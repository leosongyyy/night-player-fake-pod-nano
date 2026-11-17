#include "ui/cassette/cassette_state.h"

#include <string.h>

/* 单一实例；静态区，不进 NVS、不做持久化。 */
static cas_state_t s_state;

static void cas_state_init_defaults(void)
{
    memset(&s_state, 0, sizeof(s_state));

    /* P0 示例初值（全部保留"（示例）"文案于 UI 层，这里只存数值） */
    /* 与 Web 原型 const S 初值逐项一致（742~750 行）：
       mode=play / track=0 / pos=84 / side=甲 / bri=85 / vol=70 /
       bt=true / wifi=true / eq=0 / alarm=true。 */
    s_state.play_mode  = 1;      /* 播放（Web: mode:"play"，play 键点亮） */
    s_state.track_idx  = 0;
    s_state.pos_s      = 84;     /* 01:24 */
    s_state.side       = 0;      /* 甲面 */
    s_state.bri_pct    = 85;
    s_state.vol_pct    = 70;
    s_state.bt         = true;   /* 已连接 */
    s_state.wifi       = true;   /* 已连接 */
    s_state.eq_idx     = 0;
    s_state.keytone    = true;
    s_state.backlight  = true;
    s_state.sleep_idx  = 0;      /* 关闭 */
    s_state.weather_day = 0;     /* 今日 */
    s_state.cmode      = 0;      /* 实时时钟 */
    s_state.alarm      = true;   /* 机械闹钟拨杆默认开（Web: alarm:true） */
    s_state.sw.run     = false;
    s_state.sw.acc_ms  = 0;
    s_state.sw.last_ms = 0;

    strncpy(s_state.path[0], "根目录", CAS_PATH_MAX - 1);
    strncpy(s_state.path[1], "磁带音乐库", CAS_PATH_MAX - 1);
    s_state.path_len   = 2;
    s_state.sel        = -1;
    s_state.detail     = false;
}

cas_state_t *cas_state_get(void)
{
    return &s_state;
}

void cas_state_reset(void)
{
    cas_state_init_defaults();
}
