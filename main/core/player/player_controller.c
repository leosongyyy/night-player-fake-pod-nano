#include "core/player/player_controller.h"

#include <stdio.h>
#include <string.h>

#include "core/storage/playlist.h"
#include "esp_log.h"
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

/* 命令队列长度（控制命令都很短，队列够深避免高频点按丢弃） */
#define PLAYER_QUEUE_LEN 8

/* ---------- 命令定义 ---------- */

typedef enum {
    CMD_PLAY = 0,   /* 播放：暂停中恢复 / 未播则从头播当前曲 */
    CMD_PAUSE,      /* 暂停 */
    CMD_TOGGLE,     /* 播放/暂停切换 */
    CMD_NEXT,       /* 下一曲（手动，按循环模式处理边界） */
    CMD_PREV,       /* 上一曲（手动） */
    CMD_SEEK,       /* 跳转（arg=秒） */
    CMD_PLAY_SONG,  /* 播放指定全局下标歌曲（arg=下标） */
    CMD_FINISHED,   /* 当前曲自然播完 → 按循环模式自动续播 */
    CMD_STOP,       /* 停止 */
} player_cmd_t;

typedef struct {
    player_cmd_t cmd;
    uint32_t arg;
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

/* 手动"下一曲"：总是前进到相邻曲目；仅在"全部循环"时到达整库末尾会绕回开头，
   其余模式（单曲/列表）在整库末尾则停在原地（不越界）。跨列表前进是自然的。 */
static uint32_t player_manual_next(void)
{
    if (s_song_count == 0) {
        return s_cur_global;
    }
    if (s_cur_global < s_song_count - 1) {
        return s_cur_global + 1;
    }
    return (s_loop == PLAYER_LOOP_ALL) ? 0 : s_cur_global;
}

/* 手动"上一曲"：总是后退到相邻曲目；仅在"全部循环"时到达整库开头会绕回末尾，
   其余模式在整库开头则停在原地。跨列表后退是自然的。 */
static uint32_t player_manual_prev(void)
{
    if (s_song_count == 0) {
        return s_cur_global;
    }
    if (s_cur_global > 0) {
        return s_cur_global - 1;
    }
    return (s_loop == PLAYER_LOOP_ALL) ? (s_song_count - 1) : s_cur_global;
}

/* 歌曲自然播完后的自动续播位置：
   - 单曲循环：循环当前曲
   - 列表循环：在当前列表内前进，播到列表末尾则回到本列表第一首
   - 全部循环：前进，到达整库末尾则绕回开头（同手动"下一曲"） */
static uint32_t player_auto_next(void)
{
    if (s_song_count == 0) {
        return s_cur_global;
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
    player_cmd_msg_t m = { cmd, arg };
    /* 非阻塞投递：队列满则丢弃（控制器正忙时丢掉一两条控制命令影响不大），
       避免在高频点按或切歌时阻塞调用方（LVGL 等）线程 */
    if (xQueueSend(s_queue, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "command queue full, drop cmd=%d", (int)cmd);
    }
}

/* 播放停止回调（在播放任务 Core1 上下文执行）：
   仅当"自然播完"（is_from_user=false）时才通知控制器自动续播；
   用户主动切换（下一曲/上一曲/选曲/停止）触发的停止由已投递的命令处理，
   这里忽略，避免重复跳曲。 */
static void player_stopped_cb(bool is_from_user)
{
    if (is_from_user) {
        return;
    }
    player_post_cmd(CMD_FINISHED, 0);
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

/* 控制器任务：串行处理命令队列中的所有控制请求 */
static void player_task(void *arg)
{
    (void)arg;
    player_cmd_msg_t msg;

    while (true) {
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (msg.cmd) {
        case CMD_PLAY:       player_cmd_play(); break;
        case CMD_PAUSE:      player_cmd_pause(); break;
        case CMD_TOGGLE:     player_cmd_toggle(); break;
        case CMD_NEXT:       player_play_global(player_manual_next()); break;
        case CMD_PREV:       player_play_global(player_manual_prev()); break;
        case CMD_SEEK:       mp3_player_seek(msg.arg); break;
        case CMD_PLAY_SONG:  player_play_global(msg.arg); break;
        case CMD_FINISHED:   player_play_global(player_auto_next()); break;
        case CMD_STOP:       mp3_player_stop(); break;
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

    s_ready = true;

    /* 默认从第一个目录的第一首音乐开始播放 */
    ESP_LOGI(TAG, "controller ready, dirs=%u songs=%u, auto play first",
             (unsigned)s_dir_count, (unsigned)s_song_count);
    player_post_cmd(CMD_PLAY_SONG, 0);
    return ESP_OK;
}

bool player_controller_is_ready(void)
{
    return s_ready;
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
}

player_loop_mode_t player_controller_get_loop_mode(void)
{
    return s_loop;
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

    snprintf(out->dir_name, sizeof(out->dir_name), "%s", s_dir_name);
    snprintf(out->song_name, sizeof(out->song_name), "%s", s_song_name);
}
