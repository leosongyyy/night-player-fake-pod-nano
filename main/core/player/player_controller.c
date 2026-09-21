#include "core/player/player_controller.h"

#include <stdio.h>
#include <string.h>

#include "core/hardware/audio/audio.h"
#include "core/music/song_meta.h"
#include "core/player/shuffle_player.h"
#include "core/storage/config_store.h"
#include "core/storage/playlist.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "player_ctrl";

/* 完整播放路径缓冲大小（"/sdcard/" + 相对路径 + "/" + 文件名） */
#define PLAYER_PATH_MAX 512

/* 控制器任务参数：独立任务固定放 Core0。mp3_player_play() 在切歌时会
   阻塞等待旧曲释放资源（最长 2s），放在独立任务里才不会卡住 LVGL/UI
   所在的线程；解码播放本身在 Core1 的播放任务里，不与此任务竞争。 */
#define PLAYER_TASK_STACK 4096
#define PLAYER_TASK_PRIO  5
#define PLAYER_TASK_CORE  0

/* 连续坏曲自动跳过保护：坏曲（文件不存在/数据损坏/格式不支持）自然
   播完时自动切下一曲；若连续达到上限（如整目录/整卡文件系统损坏）则停止
   自动续播，避免在只有坏曲的目录里原地无限重播空转（也顺带避免快速切歌
   风暴反复触发 UI 封面/标签刷新），并向 UI 上报 PLAYER_EVENT_TOO_MANY_BAD */
#define PLAYER_BAD_SKIP_LIMIT 16u
static volatile uint32_t s_bad_skip_count = 0;   /* 连续坏曲计数（控制器任务上下文读写） */

/* 坏曲事件槽（UI 轮询拉取）：单槽、后写覆盖前写、拉取即清空。
   ⚠ 跨任务共享：写方=控制器任务（Core0）、读方=LVGL 任务（Core0 的
   player_ticker timer），用临界区保护（含 64 位时间戳的非原子读写） */
static portMUX_TYPE s_event_mux = portMUX_INITIALIZER_UNLOCKED;
static player_event_type_t s_bad_event = PLAYER_EVENT_NONE;   /* 当前待拉取事件 */
static int64_t s_bad_event_time_us = 0;                       /* 事件发生时刻（esp_timer us） */

/* 写入坏曲事件（后写覆盖前写；控制器任务上下文调用） */
static void player_set_bad_event(player_event_type_t ev)
{
    taskENTER_CRITICAL(&s_event_mux);
    s_bad_event = ev;
    s_bad_event_time_us = esp_timer_get_time();
    taskEXIT_CRITICAL(&s_event_mux);
}

/* 停止原因 → 坏曲事件类型（mp3_player 内部错误原因与 UI 展示类别对应） */
static player_event_type_t player_reason_to_event(mp3_player_stop_reason_t reason)
{
    switch (reason) {
    case MP3_STOP_FILE_MISSING:       return PLAYER_EVENT_FILE_MISSING;
    case MP3_STOP_FORMAT_UNSUPPORTED: return PLAYER_EVENT_FORMAT_UNSUPPORTED;
    case MP3_STOP_DATA_CORRUPT:
    default:                          return PLAYER_EVENT_DATA_CORRUPT;
    }
}

/* 命令队列长度（控制命令都很短，队列够深避免高频点按丢弃） */
#define PLAYER_QUEUE_LEN 8

/* 控制器任务队列空闲等待超时（ms）：命令队列本身是"阻塞等待"模型，用一个
   有限的超时顺带充当低频轮询点——目前用于回放增益开关的立即生效同步
   （开关状态只在 config_store 一处，见 s_gain_q16 注释）。取 500ms 与
   UI 侧的 player_ticker 周期一致，切换开关后最迟半秒内音频链路生效。 */
#define PLAYER_TASK_TICK_MS 500

/* ---------- 命令定义 ---------- */

typedef enum {
    CMD_PLAY = 0,   /* 播放：暂停中恢复 / 未播则从头播当前曲 */
    CMD_PAUSE,      /* 暂停 */
    CMD_TOGGLE,     /* 播放/暂停切换 */
    CMD_NEXT,       /* 下一曲（手动，按循环模式处理边界） */
    CMD_PREV,       /* 上一曲（手动） */
    CMD_SEEK,       /* 跳转（arg=秒） */
    CMD_PLAY_SONG,  /* 播放指定全局下标歌曲（arg=下标；随机模式下重新生成随机位置） */
    CMD_PLAY_SONG_RANDOM, /* 随机模式按指定随机位置播放（arg64=位置，不重新生成；
                             用于开机恢复随机序列现场） */
    CMD_SET_SHUFFLE,/* 设置随机播放开关（arg=0/1） */
    CMD_FINISHED,   /* 播放结束（自然播完/坏曲/无法播放，arg=mp3_player_stop_reason_t） */
    CMD_STOP,       /* 停止 */
} player_cmd_t;

typedef struct {
    player_cmd_t cmd;
    uint32_t arg;    /* 通用 32 位参数 */
    uint64_t arg64;  /* 64 位参数（随机位置），仅在 CMD_PLAY_SONG_RANDOM 使用 */
} player_cmd_msg_t;

/* ---------- 播放器状态（静态变量，供前端通过 get_status 读取） ---------- */

static volatile player_loop_mode_t s_loop = PLAYER_LOOP_LIST; /* 默认列表循环 */
static volatile bool s_ready = false;

/* 播放列表概览（初始化后固定） */
static uint32_t s_dir_count = 0;
static uint32_t s_song_count = 0;

/* 当前播放位置（由控制器任务在切歌时更新，UI 只读） */
static volatile uint32_t s_cur_global = 0;      /* 当前歌曲全局下标 */
static volatile uint32_t s_cur_dir = 0;         /* 当前目录/歌单下标 */
static volatile uint32_t s_cur_song_in_dir = 0; /* 当前歌曲在目录内序号 */
static volatile uint32_t s_dir_song_count = 0;  /* 当前目录歌曲总数 */

/* 当前曲目的名字/路径（仅控制器任务写入） */
static char s_dir_name[128];
static char s_song_name[256];
static char s_path_buf[PLAYER_PATH_MAX];

/* 当前曲目的元数据（来自 .npd 文件：标题/副标题，仅控制器任务写入；
   JPEG 封面不在此保留——解码放 UI 侧，需要时重新读文件） */
static char s_meta_title[128];
static char s_meta_subtitle[128];

/* 当前曲目的回放增益（.npd 元数据，仅控制器任务写入；UI 经 status 快照读）：
   - s_gain_q16        = 元数据里的真实增益（16.16 定点），与回放增益开关无关；
   - s_gain_known      = 当前曲是否有增益数据（false = 该曲没被扫描过、
                         没有 .npd 元数据 → UI 显示"无增益"）
   ⚠ 开关状态只存在 config_store 一处，不在这里做镜像：应用时（播放静止点与
     开关同步）都由 config_store_get_replay_gain() 现场决定用真实值还是 0dB，
     避免"两份开关状态不一致"导致的音量错乱。 */
static volatile uint32_t s_gain_q16 = AUDIO_REPLAY_GAIN_UNITY;
static volatile bool     s_gain_known = false;

/* 随机播放状态（s_shuffle 由 NVS 恢复；s_shuffle_pos 为随机序列的逻辑位置，
   仅控制器任务写入；shuffle_player 缓存懒加载。
   ⚠ 随机序列**固定基于当前播放目录**（与循环模式无关）：随机位置 P 映射到
   "当前目录内的歌曲索引"（0..dir_song_count-1），再换算全局下标
   = dir_start + 索引；s_shuffle_dir 记录当前随机列表对应的目录，只在
   用户从其它目录选歌（重新随机）时切换。 */
static volatile bool     s_shuffle        = false; /* 随机播放开关 */
static volatile bool     s_shuffle_valid  = false; /* 随机位置是否已初始化 */
static volatile uint64_t s_shuffle_pos    = 0;     /* 当前随机逻辑位置（uint64 无限） */
static volatile uint32_t s_shuffle_dir    = 0;     /* 当前随机列表所在目录下标 */
static bool              s_shuffle_inited = false; /* shuffle_player 缓存已分配 */

/* 命令队列与控制器任务句柄 */
static QueueHandle_t s_queue = NULL;
static TaskHandle_t s_task = NULL;

/* ------------------------------------------------------------------ */
/* 位置计算辅助                                                        */
/* ------------------------------------------------------------------ */

/* 由全局歌曲下标反查所在目录下标（顺序线性查找，目录数有限） */
static uint32_t player_find_dir(uint32_t global)
{
    for (uint32_t i = 0; i < s_dir_count; i++) {
        const playlist_dir_entry_t *d = playlist_dir_at(i);
        if (global >= d->song_start && global < d->song_start + d->song_count) {
            return i;
        }
    }
    return 0;
}

/* ---------- 随机播放位置辅助 ---------- */

/* 随机序列"下一曲"逻辑位置：无限序列直接 +1（永不越界）；
   位置未初始化时（异常防御）先生成随机起点 */
static uint64_t player_shuffle_next_pos(void)
{
    if (!s_shuffle_valid) {
        s_shuffle_pos = shuffle_player_random_start();
        s_shuffle_valid = true;
    }
    return s_shuffle_pos + 1;
}

/* 随机序列"上一曲"逻辑位置：后退一步；到达 0 后钳位（无限序列无"末尾"） */
static uint64_t player_shuffle_prev_pos(void)
{
    if (!s_shuffle_valid) {
        s_shuffle_pos = shuffle_player_random_start();
        s_shuffle_valid = true;
    }
    return (s_shuffle_pos > 0) ? s_shuffle_pos - 1 : 0;
}

/* 记录当前随机位置（播放新歌后调用，保证 position 与当前歌曲同步） */
static void player_shuffle_set_pos(uint64_t pos)
{
    s_shuffle_pos = pos;
    s_shuffle_valid = true;
}

/* 随机模式"下一曲"的歌曲全局下标：随机位置 +1 → 当前目录内索引 → 全局下标。
   随机序列基于当前目录（s_shuffle_dir），永不出目录（position % N 恒在目录内）。 */
static uint32_t player_shuffle_next_global(void)
{
    player_shuffle_set_pos(player_shuffle_next_pos());
    const playlist_dir_entry_t *dir = playlist_dir_at(s_shuffle_dir);
    uint32_t in_dir = (uint32_t)shuffle_player_get_index_at(s_shuffle_pos);
    if (in_dir >= dir->song_count) {
        return dir->song_start;   /* 防御：索引越界时退回目录第一首 */
    }
    return dir->song_start + in_dir;
}

/* 随机模式"上一曲"的歌曲全局下标（同下曲：基于当前目录） */
static uint32_t player_shuffle_prev_global(void)
{
    player_shuffle_set_pos(player_shuffle_prev_pos());
    const playlist_dir_entry_t *dir = playlist_dir_at(s_shuffle_dir);
    uint32_t in_dir = (uint32_t)shuffle_player_get_index_at(s_shuffle_pos);
    if (in_dir >= dir->song_count) {
        return dir->song_start;
    }
    return dir->song_start + in_dir;
}

/* 确保随机列表缓存基于指定目录（N = 该目录歌曲数）。
   返回 ESP_OK；目录为空或缓存分配失败返回错误码（调用方决定降级）。 */
static esp_err_t player_shuffle_ensure_dir(uint32_t dir)
{
    if (dir >= s_dir_count) {
        return ESP_ERR_INVALID_ARG;
    }
    const playlist_dir_entry_t *d = playlist_dir_at(dir);
    if (d->song_count == 0) {
        return ESP_ERR_INVALID_ARG;   /* 空目录无法随机 */
    }
    if (!s_shuffle_inited || s_shuffle_dir != dir) {
        esp_err_t r = shuffle_player_init(d->song_count);
        if (r != ESP_OK) {
            return r;
        }
        s_shuffle_inited = true;
        s_shuffle_dir = dir;
        ESP_LOGI(TAG, "shuffle list -> dir %u (%u songs)",
                 (unsigned)dir, (unsigned)d->song_count);
    }
    return ESP_OK;
}

/* 重新生成随机位置并定位到指定歌曲：
   目标歌曲所在目录即"当前列表"（用户从该目录选歌）；若与当前随机列表
   目录不同，先切换随机列表（重新初始化缓存），再在无限序列中随机选
   一个新块，在该块（新排列）内找到目标歌曲在目录内的偏移，
   当前位置 = 新块索引 × N + 块内偏移。之后"下一曲/上一曲"即从新序列继续。
   （"重新随机"语义：随机取新子列表 + 在该子列表内定位到当前播放的歌曲） */
static void player_shuffle_relocate(uint32_t global)
{
    if (s_song_count == 0) {
        return;
    }
    uint32_t dir = player_find_dir(global);
    const playlist_dir_entry_t *dir_e = playlist_dir_at(dir);
    uint32_t dir_count = dir_e->song_count;
    uint32_t in_dir = global - dir_e->song_start;

    /* 列表目录变化（或缓存未初始化）时先切换随机列表 */
    if (!s_shuffle_inited || s_shuffle_dir != dir) {
        esp_err_t r = player_shuffle_ensure_dir(dir);
        if (r != ESP_OK) {
            /* 缓存不可用（空目录/内存不足）：降级为顺序播放 */
            ESP_LOGE(TAG, "shuffle relocate failed (%s), fallback to sequential",
                     esp_err_to_name(r));
            s_shuffle = false;
            config_store_set_shuffle(false);
            return;
        }
    }

    uint64_t start = shuffle_player_random_start();
    uint64_t block = start / dir_count;   /* 随机块索引（block*N ≤ start，无溢出） */
    int32_t offset = shuffle_player_find_in_block(block, in_dir);
    if (offset < 0) {
        /* 理论不可达（块内包含全部歌曲）；兜底：直接用随机起点 */
        ESP_LOGW(TAG, "shuffle relocate: song #%u not found in block, use random start",
                 (unsigned)global);
        s_shuffle_pos = start;
    } else {
        s_shuffle_pos = block * dir_count + (uint64_t)offset;
    }
    s_shuffle_valid = true;
    ESP_LOGI(TAG, "shuffle relocated: dir %u song #%u -> pos %llu",
             (unsigned)dir, (unsigned)global, (unsigned long long)s_shuffle_pos);
}

/* 手动"下一曲"：总是前进到相邻曲目；仅在"全部循环"时到达整库末尾会绕回开头，
   其余模式（单曲/列表）在整库末尾则停在原地（不越界）。跨列表前进是自然的。
   随机模式下：随机序列中的下一曲（position+1），而非顺序列表的下一首。 */
static uint32_t player_manual_next(void)
{
    if (s_song_count == 0) {
        return s_cur_global;
    }
    if (s_shuffle) {
        return player_shuffle_next_global();
    }
    if (s_cur_global < s_song_count - 1) {
        return s_cur_global + 1;
    }
    return (s_loop == PLAYER_LOOP_ALL) ? 0 : s_cur_global;
}

/* 手动"上一曲"：总是后退到相邻曲目；仅在"全部循环"时到达整库开头会绕回末尾，
   其余模式在整库开头则停在原地。跨列表后退是自然的。
   随机模式下：随机序列中的上一曲（position-1，到 0 后钳位）。 */
static uint32_t player_manual_prev(void)
{
    if (s_song_count == 0) {
        return s_cur_global;
    }
    if (s_shuffle) {
        return player_shuffle_prev_global();
    }
    if (s_cur_global > 0) {
        return s_cur_global - 1;
    }
    return (s_loop == PLAYER_LOOP_ALL) ? (s_song_count - 1) : s_cur_global;
}

/* 歌曲自然播完后的自动续播位置：
   - 随机模式：随机序列无限，直接 position+1 进入下一首（单曲循环除外）
   - 单曲循环：循环当前曲
   - 列表循环：在当前列表内前进，播到列表末尾则回到本列表第一首
   - 全部循环：前进，到达整库末尾则绕回开头（同手动"下一曲"） */
static uint32_t player_auto_next(void)
{
    if (s_song_count == 0) {
        return s_cur_global;
    }
    if (s_shuffle) {
        if (s_loop == PLAYER_LOOP_SINGLE) {
            return s_cur_global;   /* 随机 + 单曲循环：原地重播 */
        }
        return player_shuffle_next_global();
    }
    if (s_loop == PLAYER_LOOP_SINGLE) {
        return s_cur_global;
    }
    if (s_loop == PLAYER_LOOP_LIST) {
        uint32_t d = player_find_dir(s_cur_global);
        const playlist_dir_entry_t *dir = playlist_dir_at(d);
        if (dir->song_count <= 1) {
            return s_cur_global;   /* 单首歌的列表：原地循环 */
        }
        uint32_t end = dir->song_start + dir->song_count - 1;
        return (s_cur_global >= end) ? dir->song_start : s_cur_global + 1;
    }
    /* 全部循环 */
    return player_manual_next();
}

/* ------------------------------------------------------------------ */
/* 元信息与播放                                                         */
/* ------------------------------------------------------------------ */

/* 把指定全局下标对应的歌曲信息填入静态变量（目录名/歌曲名/路径/位置） */
static void player_update_meta(uint32_t global)
{
    s_cur_global = global;
    s_cur_dir = player_find_dir(global);
    const playlist_dir_entry_t *dir = playlist_dir_at(s_cur_dir);
    const playlist_song_entry_t *song = playlist_song_at(global);
    s_cur_song_in_dir = global - dir->song_start;
    s_dir_song_count = dir->song_count;

    const char *dn = playlist_get_string(dir->name_str);
    const char *sn = playlist_get_string(song->name_str);
    snprintf(s_dir_name, sizeof(s_dir_name), "%s", dn ? dn : "");
    snprintf(s_song_name, sizeof(s_song_name), "%s", sn ? sn : "");
    playlist_build_path(global, s_path_buf, sizeof(s_path_buf));

    /* 解析歌曲元数据（.npd）：标题/副标题入静态缓冲，回放增益暂存到
       mp3_player（play 时在旧曲停止 + ringbuf 清空后才应用到 audio，
       避免切歌过渡期旧歌残留 PCM 用新歌增益播放的音量突变）。
       ⚠ 在控制器任务里同步读 SD 文件（几十 KB，约几十 ms），切歌本身有
       停止旧曲的等待，这里不会卡 UI；JPEG 封面数据解析后立即释放，
       封面解码放 UI 侧（now_playing 需要时重新读文件）。
       无元数据（该曲没被扫描过）时回退显示文件名/目录名，增益判为"无"，
       按 0dB 播放。 */
    s_meta_title[0] = '\0';
    s_meta_subtitle[0] = '\0';
    song_meta_t meta;
    if (song_meta_load(s_path_buf, &meta) == ESP_OK) {
        if (meta.title != NULL) {
            snprintf(s_meta_title, sizeof(s_meta_title), "%s", meta.title);
        }
        if (meta.subtitle != NULL) {
            snprintf(s_meta_subtitle, sizeof(s_meta_subtitle), "%s", meta.subtitle);
        }
        /* 缓存真实增益（元数据有效即视为"有增益数据"，扫描工具对标签缺失的
           曲目会兜底一个值，所以未扫描过的曲目才是真正的"无增益"）。
           交给 mp3_player 的 pending 值按开关决定：开关关闭时直接给 0dB，
           这样 play 的静止点一次应用到位（不必先应用真实值再覆写）。 */
        s_gain_q16 = meta.gain_q16;
        s_gain_known = true;
        mp3_player_set_gain(config_store_get_replay_gain() ? meta.gain_q16
                                                           : AUDIO_REPLAY_GAIN_UNITY);
        song_meta_free(&meta);
    } else {
        s_gain_q16 = AUDIO_REPLAY_GAIN_UNITY;
        s_gain_known = false;
        mp3_player_set_gain(AUDIO_REPLAY_GAIN_UNITY);
    }
}

/* 播放指定全局下标歌曲（更新元信息并调用播放引擎） */
static void player_play_global(uint32_t global)
{
    if (global >= s_song_count) {
        ESP_LOGW(TAG, "play index %u out of range (total %u)",
                 (unsigned)global, (unsigned)s_song_count);
        return;
    }
    player_update_meta(global);
    ESP_LOGI(TAG, "play #%u [%s] %s",
             (unsigned)global, s_dir_name, s_song_name);
    esp_err_t r = mp3_player_play(s_path_buf);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "mp3_player_play failed: %s", esp_err_to_name(r));
    }
}

/* ------------------------------------------------------------------ */
/* 命令队列与任务                                                       */
/* ------------------------------------------------------------------ */

static void player_post_cmd(player_cmd_t cmd, uint32_t arg)
{
    if (s_queue == NULL) {
        return;
    }
    player_cmd_msg_t m = { cmd, arg, 0 };
    /* 非阻塞投递：队列满则丢弃（控制器正忙时丢掉一两条控制命令影响不大），
       避免在高频点按或切歌时阻塞调用方（LVGL 等）线程 */
    if (xQueueSend(s_queue, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "command queue full, drop cmd=%d", (int)cmd);
    }
}

/* 带 64 位参数的投递（随机位置恢复） */
static void player_post_cmd64(player_cmd_t cmd, uint64_t arg64)
{
    if (s_queue == NULL) {
        return;
    }
    player_cmd_msg_t m = { cmd, 0, arg64 };
    if (xQueueSend(s_queue, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "command queue full, drop cmd=%d", (int)cmd);
    }
}

/* 播放停止回调（在播放任务 Core1 上下文执行）：
   - USER（用户主动停止/切歌触发的停止）：忽略，切歌命令已在队列中，避免重复跳曲；
   - NATURAL（自然播完）：通知控制器按循环模式自动续播；
   - FILE_MISSING / DATA_CORRUPT / FORMAT_UNSUPPORTED（坏曲/无法播放）：
     同样投递 CMD_FINISHED，控制器在 arg 里读到原因，做坏曲跳过与事件上报。 */
static void player_stopped_cb(mp3_player_stop_reason_t reason)
{
    if (reason == MP3_STOP_USER) {
        return;
    }
    player_post_cmd(CMD_FINISHED, (uint32_t)reason);
}

static void player_cmd_play(void)
{
    mp3_player_state_t st = mp3_player_get_state();
    if (st == MP3_STATE_PAUSED) {
        mp3_player_resume();
    } else if (st == MP3_STATE_IDLE) {
        player_play_global(s_cur_global);
    }
}

static void player_cmd_pause(void)
{
    if (mp3_player_get_state() == MP3_STATE_PLAYING) {
        mp3_player_pause();
        /* 暂停时保存当前歌曲（NightPlayer 原版机制：不记忆秒数位置，
           开机从歌曲开头开始）；随机模式下同步保存随机位置，
           保证断点续播回到同一序列 */
        config_store_set_last_song(s_cur_global, 0);
        if (s_shuffle) {
            config_store_set_shuffle_pos(s_shuffle_pos);
        }
    }
}

/* 手动切歌/自动续播后保存新曲位置（从头开始）：
   保证用户"切走之后直接关机/断电"也能在下次开机恢复到最后听的歌。
   注意：开机恢复播放走的是 CMD_PLAY_SONG/CMD_PLAY_SONG_RANDOM，不在此列。
   随机模式下同时保存随机位置（切歌后 position 已推进到新歌）。 */
static void player_save_new_song(void)
{
    config_store_set_last_song(s_cur_global, 0);
    if (s_shuffle) {
        config_store_set_shuffle_pos(s_shuffle_pos);
    }
}

static void player_cmd_toggle(void)
{
    mp3_player_state_t st = mp3_player_get_state();
    if (st == MP3_STATE_PLAYING) {
        mp3_player_pause();
    } else if (st == MP3_STATE_PAUSED) {
        mp3_player_resume();
    } else {
        player_play_global(s_cur_global);
    }
}

/* 切换随机播放开关（控制器任务上下文）：
   - 关闭→开启：重新生成随机位置并定位到当前歌曲（"关闭并重新开始随机
     播放"），随机列表固定为当前歌曲所在目录（内部按目录懒初始化缓存；
     空目录/内存不足会降级回顺序播放并保持关闭），立即持久化随机位置，
     之后"下一曲/上一曲"从新随机序列继续；
   - 开启→关闭：切回顺序播放，当前歌曲不变；
   开关经 config_store 持久化并通知 UI（设置页图标同步）。 */
static void player_cmd_set_shuffle(bool on)
{
    if (on == s_shuffle) {
        return;   /* 状态未变：无需处理 */
    }
    if (on) {
        s_shuffle = true;
        /* 重新生成随机位置并定位到当前歌曲（内部按当前目录切换随机列表） */
        player_shuffle_relocate(s_cur_global);
        if (!s_shuffle) {
            /* relocate 内部降级（空目录/内存不足）：持久化保持关闭，UI 同步 */
            ESP_LOGE(TAG, "shuffle enable failed, keep sequential");
            config_store_set_shuffle(false);
            return;
        }
        config_store_set_shuffle_pos(s_shuffle_pos);
    } else {
        s_shuffle = false;
    }
    config_store_set_shuffle(on);
    ESP_LOGI(TAG, "shuffle -> %d", (int)on);
}

/* 控制器任务：串行处理命令队列中的所有控制请求 */
static void player_task(void *arg)
{
    (void)arg;
    player_cmd_msg_t msg;

    /* 上次同步的回放增益开关值：用于在队列空闲（超时）时发现开关变化 */
    bool rg_last = config_store_get_replay_gain();

    while (true) {
        /* ⚠ 带超时等待：超时（无命令）时顺带做一次"回放增益开关"同步检查，
           这样开关切换能立即在音频链路上生效，而不必等到下一曲。 */
        if (xQueueReceive(s_queue, &msg,
                          pdMS_TO_TICKS(PLAYER_TASK_TICK_MS)) != pdTRUE) {
            if (config_store_get_replay_gain() != rg_last) {
                rg_last = config_store_get_replay_gain();
                if (!rg_last) {
                    /* 关闭：立即把当前曲的增益切回 0dB（直接切换，无过渡） */
                    audio_set_replay_gain(AUDIO_REPLAY_GAIN_UNITY);
                } else if (s_gain_known) {
                    /* 重新开启：恢复当前曲的真实增益值 */
                    audio_set_replay_gain(s_gain_q16);
                } else {
                    audio_set_replay_gain(AUDIO_REPLAY_GAIN_UNITY);
                }
                ESP_LOGI(TAG, "replay gain -> %d (applied %.3f)",
                         (int)rg_last,
                         (double)(rg_last && s_gain_known
                                      ? s_gain_q16 : AUDIO_REPLAY_GAIN_UNITY)
                             / 65536.0);
            }
            continue;
        }
        switch (msg.cmd) {
        case CMD_PLAY:       player_cmd_play(); break;
        case CMD_PAUSE:      player_cmd_pause(); break;
        case CMD_TOGGLE:     player_cmd_toggle(); break;
        case CMD_NEXT:       player_play_global(player_manual_next()); player_save_new_song(); break;
        case CMD_PREV:       player_play_global(player_manual_prev()); player_save_new_song(); break;
        case CMD_SEEK:       mp3_player_seek(msg.arg); break;
        case CMD_PLAY_SONG:
            /* 用户从列表指定播放（或顺序模式开机恢复）：
               随机模式下视为"重新随机"——重新生成随机位置并定位到该歌曲 */
            if (s_shuffle) {
                player_shuffle_relocate(msg.arg);
            }
            player_play_global(msg.arg);
            break;
        case CMD_PLAY_SONG_RANDOM: {
            /* 随机模式恢复现场：按保存的随机位置播放（不重新生成）。
               随机列表目录（s_shuffle_dir）已由 init 设置，位置 P → 目录内
               索引 → 全局下标。位置无效（异常）时退回顺序播放第一首。 */
            if (s_shuffle && s_shuffle_inited && s_shuffle_dir < s_dir_count) {
                const playlist_dir_entry_t *dir = playlist_dir_at(s_shuffle_dir);
                int32_t in_dir = shuffle_player_get_index_at(msg.arg64);
                if (in_dir >= 0 && (uint32_t)in_dir < dir->song_count) {
                    s_shuffle_pos = msg.arg64;
                    s_shuffle_valid = true;
                    player_play_global(dir->song_start + (uint32_t)in_dir);
                } else {
                    ESP_LOGW(TAG, "resume shuffle pos invalid, play first song");
                    player_play_global(0);
                }
            } else {
                player_play_global(0);
            }
            break;
        }
        case CMD_SET_SHUFFLE: player_cmd_set_shuffle(msg.arg != 0); break;
        case CMD_FINISHED: {
            /* 播放结束收口（arg = mp3_player_stop_reason_t）：
               - NATURAL：正常自然播完 → 按循环模式自动续播，清空连续坏曲计数；
               - FILE_MISSING / DATA_CORRUPT / FORMAT_UNSUPPORTED：坏曲/无法播放
                 → 上报对应事件（UI 拉取弹 toast）并自动跳过切下一曲；
                 连续坏曲达到上限 → 停止自动续播（整目录/整卡损坏防无限空转），
                 上报 PLAYER_EVENT_TOO_MANY_BAD（覆盖此前单曲事件），并记
                 "关机前未在播放"，下次开机不再自动播放进坏区。 */
            mp3_player_stop_reason_t reason = (mp3_player_stop_reason_t)msg.arg;
            /* 兜底：报"自然播完"但从未解码出任何音频（总时长仍为 0，如
               门禁后数据不足的残废文件）→ 仍按数据损坏处理，防单曲循环
               下原地无限重试（正常能播放的歌 started 后必有时长） */
            if (reason == MP3_STOP_NATURAL && mp3_player_get_duration_s() == 0) {
                reason = MP3_STOP_DATA_CORRUPT;
            }
            if (reason == MP3_STOP_NATURAL) {
                s_bad_skip_count = 0;
                player_play_global(player_auto_next());
                player_save_new_song();
                break;
            }
            /* 坏曲：上报事件（后写覆盖前写；UI 每 500ms 拉取一次并展示，
               拉取即清空 → 用户看到的是最近一次坏曲类别） */
            player_set_bad_event(player_reason_to_event(reason));
            if (++s_bad_skip_count >= PLAYER_BAD_SKIP_LIMIT) {
                s_bad_skip_count = 0;
                ESP_LOGE(TAG, "%u 首连续坏曲，疑似整目录/整卡文件损坏，"
                              "停止自动续播（reason=%d）",
                         (unsigned)PLAYER_BAD_SKIP_LIMIT, (int)reason);
                /* 覆盖为"连续坏曲过多"事件：无论何时被 UI 拉取都提示 */
                player_set_bad_event(PLAYER_EVENT_TOO_MANY_BAD);
                config_store_set_was_playing(false);
                break;   /* 保持 IDLE，不再续播 */
            }
            ESP_LOGW(TAG, "bad track (reason=%d), auto skip next (#%u/%u)",
                     (int)reason, (unsigned)s_bad_skip_count,
                     (unsigned)PLAYER_BAD_SKIP_LIMIT);
            player_play_global(player_auto_next());
            player_save_new_song();
            break;
        }
        case CMD_STOP:
            /* 停止前保存当前歌曲（NightPlayer 原版机制：不记忆秒数位置）；
               随机模式下同步保存随机位置 */
            if (mp3_player_get_state() != MP3_STATE_IDLE) {
                config_store_set_last_song(s_cur_global, 0);
                if (s_shuffle) {
                    config_store_set_shuffle_pos(s_shuffle_pos);
                }
            }
            mp3_player_stop();
            break;
        default: break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                             */
/* ------------------------------------------------------------------ */

esp_err_t player_controller_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    /* 初始化播放列表读取（内存映射 pldata 分区） */
    esp_err_t r = playlist_init();
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "playlist_init failed: %s", esp_err_to_name(r));
        return r;
    }
    s_dir_count = playlist_dir_count();
    s_song_count = playlist_song_count();
    if (s_song_count == 0) {
        ESP_LOGW(TAG, "no songs in playlist");
        playlist_deinit();
        return ESP_ERR_NOT_FOUND;
    }

    /* 创建命令队列与控制器任务（Core0） */
    s_queue = xQueueCreate(PLAYER_QUEUE_LEN, sizeof(player_cmd_msg_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "create command queue failed");
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ret = xTaskCreatePinnedToCore(player_task, "player_ctrl",
                                             PLAYER_TASK_STACK, NULL,
                                             PLAYER_TASK_PRIO, &s_task,
                                             PLAYER_TASK_CORE);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "create controller task failed");
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* 注册"自然播完"回调 */
    mp3_player_set_stopped_callback(player_stopped_cb);

    /* 恢复用户循环模式（NVS 持久化） */
    uint8_t saved_loop = config_store_get_loop_mode();
    if (saved_loop <= PLAYER_LOOP_LIST) {
        s_loop = (player_loop_mode_t)saved_loop;
    } else {
        s_loop = PLAYER_LOOP_LIST;
    }

    s_ready = true;
    s_bad_skip_count = 0;   /* 每次初始化清空坏曲计数 */

    /* 读取上次播放歌曲（NVS 持久化），供恢复随机列表目录 / 恢复播放用。
       ⚠ NightPlayer 原版机制：只记忆歌曲，不记忆秒数位置（开机从歌曲
       开头开始），last_pos 已不再使用。 */
    uint32_t last_global = 0;
    uint32_t last_pos = 0;   /* 保留接口参数，值为 0（不记忆秒数） */
    bool has_last = config_store_get_last_song(&last_global, &last_pos);

    /* 关机前播放状态记忆（NVS）：false=伪关机前已暂停/停止 → 开机只恢复
       当前歌曲不自动播放，随后紧跟 CMD_PAUSE 停在暂停态；
       true=关机前在播放（或无记录，默认）→ 开机自动播放（旧行为）。 */
    const bool resume_paused = !config_store_get_was_playing();

    /* 恢复随机播放开关（NVS）：开启时按"当前列表"（上次播放歌曲所在目录）
       初始化随机列表缓存，并恢复/生成随机位置。缓存分配失败则降级为
       顺序播放（持久化开关同步回写，保证设置页图标与实际行为一致）。 */
    s_shuffle = config_store_get_shuffle();
    if (s_shuffle) {
        /* 确定随机列表目录：优先用上次播放歌曲所在目录（保存现场一致）；
           无记录时用第一个目录 */
        uint32_t shuffle_dir = 0;
        if (has_last && last_global < s_song_count) {
            shuffle_dir = player_find_dir(last_global);
        }
        if (player_shuffle_ensure_dir(shuffle_dir) != ESP_OK) {
            ESP_LOGE(TAG, "shuffle list init failed (dir %u), disable shuffle",
                     (unsigned)shuffle_dir);
            s_shuffle = false;
            config_store_set_shuffle(false);
        }
    }

    /* 默认从上次播放歌曲恢复（NVS 持久化）：切到上次歌曲后从开头播放
       （不记忆秒数位置，NightPlayer 原版机制）。无记录或下标越界（SD 卡
       换过）时播放第一首。命令队列串行处理，顺序正确。
       随机模式下：优先用保存的随机位置（序列一致，回到同一首歌、同一目录）；
       无有效记录则生成随机起点（首次开启后未播过 / SD 卡数据变化）。 */
    ESP_LOGI(TAG, "controller ready, dirs=%u songs=%u, loop=%d, shuffle=%d",
             (unsigned)s_dir_count, (unsigned)s_song_count, (int)s_loop,
             (int)s_shuffle);

    if (s_shuffle) {
        const playlist_dir_entry_t *dir = playlist_dir_at(s_shuffle_dir);
        uint64_t saved_pos = 0;
        bool has_pos = config_store_get_shuffle_pos(&saved_pos);
        int32_t in_dir = has_pos ? shuffle_player_get_index_at(saved_pos) : -1;
        if (in_dir >= 0 && (uint32_t)in_dir < dir->song_count) {
            /* 恢复保存的随机位置（不重新生成，序列与现场完全一致） */
            s_shuffle_valid = true;
            s_shuffle_pos = saved_pos;
            ESP_LOGI(TAG, "resume shuffle pos %llu -> dir %u song #%u",
                     (unsigned long long)saved_pos, (unsigned)s_shuffle_dir,
                     (unsigned)(dir->song_start + (uint32_t)in_dir));
            player_post_cmd64(CMD_PLAY_SONG_RANDOM, saved_pos);
        } else {
            /* 无记录或位置越界（SD 卡换过）：生成随机起点并立即保存 */
            s_shuffle_pos = shuffle_player_random_start();
            s_shuffle_valid = true;
            ESP_LOGI(TAG, "shuffle new random start pos %llu (dir %u)",
                     (unsigned long long)s_shuffle_pos, (unsigned)s_shuffle_dir);
            player_post_cmd64(CMD_PLAY_SONG_RANDOM, s_shuffle_pos);
            config_store_set_shuffle_pos(s_shuffle_pos);
        }
        /* 关机前已暂停：恢复当前歌曲后立即暂停（不自动出声） */
        if (resume_paused) {
            player_post_cmd(CMD_PAUSE, 0);
        }
    } else if (has_last && last_global < s_song_count) {
        ESP_LOGI(TAG, "resume last song #%u%s",
                 (unsigned)last_global,
                 resume_paused ? " (paused, no auto play)" : "");
        player_post_cmd(CMD_PLAY_SONG, last_global);
        /* 关机前已暂停：恢复当前歌曲后立即暂停（不自动出声） */
        if (resume_paused) {
            player_post_cmd(CMD_PAUSE, 0);
        }
    } else {
        ESP_LOGI(TAG, "no last song record, play first song");
        player_post_cmd(CMD_PLAY_SONG, 0);
    }
    return ESP_OK;
}

bool player_controller_is_ready(void)
{
    return s_ready;
}

/* 释放播放控制器（见 player_controller.h 注释）。
   顺序（不可颠倒）：
   1. 停止播放并等待解码任务退出（释放文件句柄/I2S）；
   2. 取消"自然播完"回调（防止播放任务向已删队列投递自动续播命令）；
   3. 挂起控制器任务：此后不会再处理命令、不会再访问 playlist 指针
      （⚠ player_update_meta 等对 playlist 指针无判空，deinit 后任务
      若继续跑会在 playlist_deinit 后的空指针上崩溃）；
   4. 删除命令队列（player_post_cmd 对空队列已判空返回，安全）；
   5. 卸载播放列表（munmap + 清空指针）——必须在任务挂起之后，
      否则正在跑的任务会访问失效映射。 */
void player_controller_deinit(void)
{
    /* 1. 停止播放：若当前有曲目，先停并等解码任务退出（最多 1s） */
    if (mp3_player_get_state() != MP3_STATE_IDLE) {
        mp3_player_stop();
        uint32_t wait = 0;
        while (mp3_player_get_state() != MP3_STATE_IDLE && wait < 1000) {
            vTaskDelay(pdMS_TO_TICKS(10));
            wait += 10;
        }
    }

    /* 2. 取消自然播完回调（避免播放任务向已删队列投递 CMD_FINISHED） */
    mp3_player_set_stopped_callback(NULL);

    /* 3. 挂起控制器任务（Core0，可能阻塞在 xQueueReceive） */
    if (s_task != NULL) {
        vTaskSuspend(s_task);
    }

    /* 4. 删除命令队列 */
    if (s_queue != NULL) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }

    /* 5. 卸载播放列表（munmap + 清空指针）。
       ⚠ 必须最后执行：此前所有对 playlist 指针的访问都已停止
       （播放已停、任务已挂起），此后再调用 playlist_* 返回空/0 安全。 */
    playlist_deinit();

    s_ready = false;
    ESP_LOGI(TAG, "player controller released (playback stopped, task suspended, "
                  "queue deleted, playlist unmapped)");
}

void player_controller_play(void)
{
    player_post_cmd(CMD_PLAY, 0);
}

void player_controller_pause(void)
{
    player_post_cmd(CMD_PAUSE, 0);
}

void player_controller_toggle(void)
{
    player_post_cmd(CMD_TOGGLE, 0);
}

void player_controller_next(void)
{
    player_post_cmd(CMD_NEXT, 0);
}

void player_controller_prev(void)
{
    player_post_cmd(CMD_PREV, 0);
}

void player_controller_seek(uint32_t target_s)
{
    player_post_cmd(CMD_SEEK, target_s);
}

void player_controller_play_song(uint32_t global_idx)
{
    player_post_cmd(CMD_PLAY_SONG, global_idx);
}

void player_controller_stop(void)
{
    player_post_cmd(CMD_STOP, 0);
}

void player_controller_set_loop_mode(player_loop_mode_t mode)
{
    if (mode > PLAYER_LOOP_LIST) {
        mode = PLAYER_LOOP_LIST;
    }
    s_loop = mode;
    /* 同步持久化到 NVS，开机自动恢复 */
    config_store_set_loop_mode((uint8_t)mode);
}

player_loop_mode_t player_controller_get_loop_mode(void)
{
    return s_loop;
}

void player_controller_set_shuffle(bool on)
{
    player_post_cmd(CMD_SET_SHUFFLE, on ? 1 : 0);
}

bool player_controller_get_shuffle(void)
{
    return s_shuffle;
}

void player_controller_save_last_position(void)
{
    if (!s_ready) {
        return;
    }
    /* ⚠ NightPlayer 原版机制：只记忆当前歌曲，不记忆秒数位置
       （pos 恒为 0，开机从歌曲开头开始） */
    config_store_set_last_song(s_cur_global, 0);
    /* 记录关机前播放状态（NVS）：true=当时在播放（开机自动播放），
       false=已暂停/停止（开机只恢复当前歌曲不自动播放，避免突然出声）。
       与当前歌曲一起保存，供 player_controller_init 恢复。 */
    config_store_set_was_playing(mp3_player_get_state() == MP3_STATE_PLAYING);
    /* 随机模式下同步保存随机位置（关机前最后状态，开机完整恢复序列） */
    if (s_shuffle) {
        config_store_set_shuffle_pos(s_shuffle_pos);
    }
    ESP_LOGI(TAG, "save last position: song #%u was_playing=%d%s",
             (unsigned)s_cur_global,
             (int)(mp3_player_get_state() == MP3_STATE_PLAYING),
             s_shuffle ? " (shuffle)" : "");
}

void player_controller_get_status(player_controller_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));

    out->loop_mode = s_loop;
    out->dir_count = s_dir_count;
    out->total_song_count = s_song_count;
    out->cur_global = s_cur_global;
    out->cur_dir = s_cur_dir;
    out->cur_song_in_dir = s_cur_song_in_dir;
    out->dir_song_count = s_dir_song_count;
    out->playing = mp3_player_get_state();
    out->position_s = mp3_player_get_position_s();
    out->duration_s = mp3_player_get_duration_s();
    out->has_song = (s_ready && s_song_count > 0);
    out->has_replay_gain = s_gain_known;
    out->replay_gain_q16 = s_gain_q16;

    snprintf(out->dir_name, sizeof(out->dir_name), "%s", s_dir_name);
    snprintf(out->song_name, sizeof(out->song_name), "%s", s_song_name);
    /* 标题/副标题：有元数据用元数据，否则回退文件名/目录名 */
    snprintf(out->title, sizeof(out->title), "%s",
             s_meta_title[0] ? s_meta_title : s_song_name);
    snprintf(out->subtitle, sizeof(out->subtitle), "%s",
             s_meta_subtitle[0] ? s_meta_subtitle : s_dir_name);
}

const char *player_controller_get_current_path(void)
{
    return s_path_buf;
}

bool player_controller_take_event(player_event_t *out)
{
    if (out == NULL) {
        return false;
    }
    taskENTER_CRITICAL(&s_event_mux);
    const bool has = (s_bad_event != PLAYER_EVENT_NONE);
    if (has) {
        out->type = s_bad_event;
        out->time_us = s_bad_event_time_us;
        s_bad_event = PLAYER_EVENT_NONE;   /* 拉取即清空（单槽） */
    }
    taskEXIT_CRITICAL(&s_event_mux);
    return has;
}
