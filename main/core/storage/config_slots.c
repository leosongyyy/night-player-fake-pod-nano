#include "core/storage/config_slots.h"

#include "core/hardware/audio/audio.h"
#include "core/hardware/audio/eq.h"
#include "core/player/player_controller.h"
#include "core/storage/config_store.h"
#include "core/storage/playlist.h"
#include "core/utils/sleep_timer.h"
#include "ui/toast.h"

#include <stdio.h>

#include "esp_log.h"

static const char *TAG = "config_slots";

/* 保存槽位时用于 UI 显示的列表名：把"最后播放歌曲"所在目录/歌单名连同槽位
   一起存下来（config_store 不依赖 playlist，故由这里取名字告诉它）。
   player_controller 的 dir_name 上限 128，与 CONFIG_STORE_SLOT_NAME_MAX 一致。 */
static void slot_note_current_list_name(void)
{
    /* 用实时状态而不是 config_store 的 last_global：切歌后 controller 已更新，
       而 config_store 的 last 也可能刚更新过，两者一致；这里以 controller 为准
       更贴近"用户看到的当前列表" */
    player_controller_status_t st;
    player_controller_get_status(&st);
    if (!st.has_song) {
        return;
    }
    config_store_set_slot_list_name(st.dir_name);
}

void config_slots_switch(uint32_t idx)
{
    if (idx >= config_store_slot_count() || idx == config_store_get_active_slot()) {
        return;   /* 越界 / 点的是当前槽位：无事可做 */
    }

    /* 1. 先记下"离开时所在列表"的名字，再存回原槽位（switch_slot 内部保存）。
          ⚠ 必须在 switch_slot 之前调用：切换后 config_store 的内存值已是新
          槽位的，列表名快照会跟着写错。 */
    slot_note_current_list_name();

    /* 2. 存储层切换：保存旧槽位 → 换活跃下标（写 NVS）→ 载入新槽位 → 通知 UI */
    config_store_switch_slot(idx);

    /* 3. 读取切换后的槽位内容（此时 config_store 的 get_xxx 已是新槽位的值）。
          摘要（list_name）另取：它只在 Toast 里用，不参与运行时应用。 */
    config_store_slot_snapshot_t snap;
    config_store_get_slot_snapshot(idx, &snap);
    config_store_slot_info_t info;
    config_store_get_slot_info(idx, &info);

    /* 4. 音量：直接落到 DAC（不等下一次播放），保证"切换即可感知" */
    audio_set_volume_level(config_store_get_volume());

    /* 5. EQ：有预设则应用，无则关闭（直通） */
    char eq_name[EQ_FILE_NAME_MAX] = "";
    eq_preset_t eq_preset;
    if (config_store_get_eq(eq_name, sizeof(eq_name), &eq_preset)) {
        eq_apply(&eq_preset);
    } else {
        eq_apply(NULL);
    }

    /* 6. 睡眠定时：按槽位里存的**原时长**重新开表（0 = 关闭） */
    sleep_timer_set(config_store_get_sleep_default_min());

    /* 7. 播放器：循环 / 随机 / 当前歌曲（越界回退第一首）并自动播放。
          控制器在 Core0 控制任务上下文原子执行，避免与切歌命令抢时序。 */
    uint32_t song_idx = 0;
    if (snap.has_data && snap.last_valid &&
        snap.last_global < playlist_song_count()) {
        song_idx = snap.last_global;
    }
    player_controller_restore_config((player_loop_mode_t)snap.loop_mode,
                                     snap.shuffle, snap.shuffle_pos_valid,
                                     snap.shuffle_pos, song_idx);

    ESP_LOGI(TAG, "switched to SAVE%u: vol=%u loop=%u shuffle=%d sleep=%u min "
                  "song=%u eq=%s",
             (unsigned)(idx + 1u), (unsigned)config_store_get_volume(),
             (unsigned)snap.loop_mode, (int)snap.shuffle,
             (unsigned)config_store_get_sleep_default_min(),
             (unsigned)song_idx, eq_name[0] ? eq_name : "none");

    /* 8. 提示用户（含该槽位的列表名，便于确认切到了哪套配置） */
    char msg[96];
    if (info.has_data && info.list_name[0] != '\0') {
        snprintf(msg, sizeof(msg), "已切换到 SAVE%u（%s）",
                 (unsigned)(idx + 1u), info.list_name);
    } else {
        snprintf(msg, sizeof(msg), "已切换到 SAVE%u", (unsigned)(idx + 1u));
    }
    ui_toast_show(msg);
}
