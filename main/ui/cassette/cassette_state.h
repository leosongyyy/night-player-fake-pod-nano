#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file cassette_state.h
 * @brief P0 全局静态状态（单一实例，纯 RAM，不落盘 / 不写 NVS）
 *
 * F16：写 flash/NVS 会关缓存停核，XIP 下 LVGL 读 PSRAM 会 Core1 fault。
 * 因此 7 个模块的所有跨屏状态都放这里，只改 RAM；只有开机早期的
 * ui_mode（cassette_fallback.c）允许写一次 NVS。
 *
 * P0 的数据均为"（示例）"静态值，P1 接真实服务时在本状态上做联动，
 * 模块不要各自持有"当前曲目"等跨屏变量。
 */

#define CAS_PATH_MAX   32u
#define CAS_PATH_DEPTH 3u

typedef struct {
    bool    run;        /* 秒表运行中 */
    uint32_t acc_ms;    /* 已累计毫秒 */
    uint32_t last_ms;   /* 本次启动时刻（esp_timer ms） */
} cas_stopwatch_t;

typedef struct {
    /* M1 磁带仓 */
    uint8_t  play_mode;    /* 0=停止 1=播放 2=暂停 */
    uint8_t  track_idx;    /* 当前曲目下标（0~4） */
    uint32_t pos_s;        /* 当前进度秒（示例 01:24 = 84） */
    uint8_t  side;         /* 0=甲面 1=乙面 */

    /* M2 快捷中心 / 抽屉 */
    uint8_t  bri_pct;      /* 亮度百分比（示例 85） */
    uint8_t  vol_pct;      /* 音量百分比（示例 70） */
    bool     bt;           /* 蓝牙开关 */
    bool     wifi;         /* 无线网络开关 */

    /* M6 设置 */
    uint8_t  eq_idx;       /* 三段均衡器档位 */
    bool     keytone;      /* 机械按键音 */
    bool     backlight;    /* 背光常亮 */
    uint8_t  sleep_idx;    /* 自动休眠档位 0=关闭 1=15分 2=30分 3=60分 */

    /* M3 天气 */
    uint8_t  weather_day;  /* 0=今日 1=明日 2=后日 */

    /* M4 时钟 */
    uint8_t  cmode;        /* 0=实时时钟 1=机械闹钟 2=秒表计时 */
    bool     alarm;        /* 闹钟拨杆开关 */
    cas_stopwatch_t sw;

    /* M7 文件管理 */
    char     path[CAS_PATH_DEPTH][CAS_PATH_MAX];  /* 面包屑层级 */
    uint8_t  path_len;
    int8_t   sel;          /* 当前选中行（-1=未选） */
    bool     detail;       /* 详情面板是否展开 */
} cas_state_t;

/** 唯一实例访问器（模块内禁止复制结构体，只拿指针读写） */
cas_state_t *cas_state_get(void);

/** 恢复到 P0 示例初值（软重启/回退到磁带页时可选调用） */
void cas_state_reset(void);

#ifdef __cplusplus
}
#endif
