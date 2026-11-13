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

void config_slots_switch(uint32_t idx)
{
    if (idx >= config_store_slot_count() || idx == config_store_get_active_slot()) {
        return;   /* 越界 / 点的是当前槽位：无事可做 */
    }

    /* 1. 先把**播放现场**同步进当前槽位：当前歌曲 + 随机位置 + 列表名。
          ⚠ 必须在 switch_slot 之前调用：切换后 config_store 的内存值已是新
          槽位的，此时再写就会写进新槽位。
          ⚠ 只靠"每次 set_last_song 都落盘"是不够的：从列表点歌走
          player_controller_play_song（命令队列异步），保存与命令的先后不
          确定；这里直接读控制器当前状态，保证存下去的是此刻在播的歌。
          （历史 bug：切走再切回还是原来那首，关机再开才对——因为关机保存
          用的是控制器实时状态。） */
    player_controller_sync_config();

    /* 2. 存储层切换：保存旧槽位 → 换活跃下标（写 NVS）→ 载入新槽位 → 通知 UI */
    config_store_switch_slot(idx);

    /* 3. 读取切换后的槽位内容（此时 config_store 的 get_xxx 已是新槽位的值）。
          ⚠ 这里**不取槽位摘要**（列表名）：Toast 只报槽位编号——目录/歌单名
          最长 128 字节，塞进底部提示框会溢出/折行；列表名在槽位弹窗里看
          （ui/config_slot_popup 自己读摘要）。 */
    config_store_slot_snapshot_t snap;
    config_store_get_slot_snapshot(idx, &snap);

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

    /* 6. 睡眠定时：**不属于配置槽位**（2026-11 起时长与开关都是全局键），
          切槽位既不改变它的时长/开关，也不重新开表——正在跑的定时继续跑，
          没开的定时不会因为切配置被打开。需要调整只在设置页/通知栏操作。 */
    ESP_LOGI(TAG, "sleep timer untouched by slot switch (def_on=%d, %u min, "
                  "active=%d)",
             (int)config_store_get_sleep_def_on(),
             (unsigned)config_store_get_sleep_default_min(),
             (int)sleep_timer_is_active());

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

    ESP_LOGI(TAG, "switched to SAVE%u: vol=%u loop=%u shuffle=%d song=%u eq=%s",
             (unsigned)(idx + 1u), (unsigned)config_store_get_volume(),
             (unsigned)snap.loop_mode, (int)snap.shuffle,
             (unsigned)song_idx, eq_name[0] ? eq_name : "none");

    /* 8. 提示用户：只报槽位编号（列表名可能很长，见第 3 步说明） */
    char msg[32];
    snprintf(msg, sizeof(msg), "已切换到 SAVE%u", (unsigned)(idx + 1u));
    ui_toast_show(msg);
}
