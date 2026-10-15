#pragma once

/**
 * @file global_style.h
 * @brief 全局 UI 风格 / 动画统一配置（目前只装跑马灯 marquee 的节奏参数）
 *
 * ============================ 跑马灯参数 ============================
 * 全项目**所有**跑马灯（目录列表行、歌曲列表行、正在播放页标题/副标题）共用
 * 下面这一组宏：先停顿 START_DELAY，再滚动 ROUNDS 轮，每轮之间停顿 PAUSE，
 * 滚完就停下（不再无限循环，避免长标题一直晃眼）。
 * 想调节奏就改这几个宏，不需要 Kconfig、也不需要动任何页面代码。
 *
 * ======================= LVGL 9 实测结论 =======================
 * v8 时代这个模块之所以存在，是因为 **8.4 的 lv_label 不认模板里的轮数**：
 * `lv_label_refr_text()` 建滚动动画时把 `repeat_cnt` 写死成
 * `LV_ANIM_REPEAT_INFINITE`，模板里只有 `act_time`（起始延迟）与
 * `repeat_delay`（轮间停顿）会被读取，所以当时必须"动画建好后就地改
 * repeat_cnt + 200ms 守护定时器兜底重建"。
 *
 * **v9 已经原生支持**：`lv_label.c` 的 `overwrite_anim_property()` 在每次
 * 重建跑马灯动画时会把模板里的
 *   · act_time      —— 起始延迟（= lv_anim_set_delay() 的负值）
 *   · repeat_cnt    —— 总轮数
 *   · repeat_delay  —— 每轮之间的停顿
 *   · completed_cb  —— 完成回调
 * 全部抄进新动画（`LV_LABEL_LONG_SCROLL` 模式还会多抄 reverse_delay）。
 * 而且它还会保留旧动画的播放位置（`act_time > 0` 时不被模板覆盖），
 * 所以文本更新后的重绑不会再把整屏跑马灯打回起点。
 *
 * 因此迁移到 9.4 后本模块大幅简化：只维护一个全局 `lv_anim_t` 模板 +
 * `lv_obj_set_style_anim()`，**删掉了就地接管与守护定时器**（连带
 * 48 项的 label 监管表）。用法与语义完全不变。
 *
 * 用法（务必在 LVGL 任务上下文调用，即已持 lvgl_port_lock 或用事件/定时器回调）：
 *
 *     lv_obj_t *lbl = lv_label_create(parent);
 *     lv_label_set_text(lbl, "很长的标题……");
 *     lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
 *     lv_obj_set_width(lbl, LV_PCT(100));
 *     global_style_marquee_attach(lbl);      // ① 创建后挂上
 *     ...
 *     global_style_marquee_set_text(lbl, "换了新标题");  // ② 每次改文本后
 *
 * 把长文本模式切到 LV_LABEL_LONG_CLIP 就等于关闭该 label 的跑马灯
 * （本模块只接管处于 CIRCULAR 模式的 label），无需手工注销。
 */

#include "lvgl.h"

/* ==================== 跑马灯节奏（想调就改这里） ==================== */

/** 总滚动轮数（含第一轮）。0 = 无限循环（退回 LVGL 原生行为） */
#define GLOBAL_STYLE_MARQUEE_ROUNDS          3

/** 每轮滚动之间的停顿（ms） */
#define GLOBAL_STYLE_MARQUEE_PAUSE_MS        1000

/** 首次滚动之前的停顿（ms）：先让用户读到标题开头再开始滚，0 = 立即滚动 */
#define GLOBAL_STYLE_MARQUEE_START_DELAY_MS  1000

/** 单轮滚动时长（ms）。0 = 沿用 LVGL 默认（约 300~10000ms 之间的默认值）
 *
 *  ⚠ v8 时代这里是 `GLOBAL_STYLE_MARQUEE_SPEED`（px/s），对应样式属性
 *  `LV_STYLE_ANIM_SPEED`；**v9 删除了 anim_speed，只保留 anim_duration
 *  （毫秒）**，语义从"速度"变成"滚完一轮的时长"，故此处换了单位与名字。
 *  默认仍为 0（不设该样式）→ 与迁移前实际行为一致。 */
#define GLOBAL_STYLE_MARQUEE_DURATION_MS     0

/**
 * @brief 把 label 纳入全局跑马灯管理：挂全局动画模板 + 按上面的参数（重新）开始滚动
 *
 * 幂等：同一个 label 可以反复调用（每次都会从头重跑一遍跑马灯）。
 *
 * @param label 必须是 lv_label；其长文本模式须为 LV_LABEL_LONG_SCROLL_CIRCULAR
 *              才会真正开始滚动（CLIP 等其它模式只挂模板、不启动）
 */
void global_style_marquee_attach(lv_obj_t *label);

/**
 * @brief 改写跑马灯 label 的文本并（内容真的变了才）按全局节奏重新开始滚动
 *
 * **改跑马灯 label 的文本请一律用它，不要直接调 lv_label_set_text()。**
 *
 * ⚠ 原因（实测坑）：`lv_label_set_text()` 即使内容**完全相同**也会走
 * `lv_label_refr_text()` 重建滚动动画——表现就是"文本没变，跑马灯却莫名
 * 回到起点、重新停顿 1 秒"。列表行尤其敏感：曲目/目录变化时上层会重绑
 * **全部可见行**（只为刷新"正在播放"的黄色高亮），一旦在重绑里裸调
 * set_text，就会出现"切歌时整屏跑马灯一起重置"。
 *
 * 本函数先比较内容：相同 → 直接返回（动画完全不动），不同 → set_text + 重跑。
 * 所以**真正的换歌依旧是"停顿 1 秒 → 滚 N 轮"**，1 秒初始延迟不受影响。
 */
void global_style_marquee_set_text(lv_obj_t *label, const char *text);
