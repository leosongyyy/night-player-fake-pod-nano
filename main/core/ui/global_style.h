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
 * ======================= LVGL 8.4 实测结论 =======================
 * 计划里提到的"用样式属性塞一个全局动画模板（LV_STYLE_ANIM）"确实存在，但在
 * **8.4 上只有一半能用**，读源码（managed_components/lvgl__lvgl/src/widgets/
 * lv_label.c 的 lv_label_refr_text）得到的事实：
 *
 *  1. 模板只对 `LV_LABEL_LONG_SCROLL_CIRCULAR` 生效
 *     （`LV_LABEL_LONG_SCROLL` 来回滚那条分支完全不看模板）。
 *  2. 模板里只有两个字段被读取：
 *       · act_time      —— 起始延迟（= lv_anim_set_delay() 的负值）
 *       · repeat_delay  —— 每轮之间的停顿
 *     模板里的 time / repeat_cnt **一概被忽略**。
 *  3. 更关键的是 `repeat_count` 被 lv_label **写死为无限**：
 *       lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
 *     也就是说官方示例 lv_example_label_5 里设的"只重复 3 次"在 8.4 里是**无效**的
 *     （智谱那份资料只对了一半），这也是本模块必须存在的理由。
 *
 * 因此本模块的做法（不修改 managed_components 里的 LVGL 源码）：
 *   ① 用一个全局 `lv_anim_t` 模板 + `lv_obj_set_style_anim()` 把"起始延迟 +
 *      轮间停顿"交给 LVGL 原生处理 —— LVGL 每次因文本/尺寸/样式变化重建滚动
 *      动画时都会自动带上这两个参数，改了文本也能正确重来；
 *   ② 动画建好之后**就地**把它改造成有限轮数（lv_anim_get() 拿到 LVGL 刚建的
 *      那条动画，改 repeat_cnt）—— 而不是另建一条自己的动画，这样同一个 label
 *      上永远只有一条滚动动画，不会出现两条动画抢写 label->offset.x 的冲突，
 *      也不会破坏 LVGL 自己的"切换长文本模式即停滚动"等逻辑；
 *   ③ 一个 200ms 的守护定时器兜底：LVGL 在文本/尺寸/样式变化时会重建动画
 *      （重建后轮数又变回无限），守护定时器发现"没被接管过"的动画就补一次。
 *
 * 用法（务必在 LVGL 任务上下文调用，即已持 lvgl_port_lock 或用事件/定时器回调）：
 *
 *     lv_obj_t *lbl = lv_label_create(parent);
 *     lv_label_set_text(lbl, "很长的标题……");
 *     lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
 *     lv_obj_set_width(lbl, LV_PCT(100));
 *     global_style_marquee_attach(lbl);      // ① 创建后挂上
 *     ...
 *     lv_label_set_text(lbl, "换了新标题");  // ② 每次改文本后
 *     global_style_marquee_attach(lbl);      //    再挂一次（重启跑马灯）
 *
 * label 被删除时无需手工注销（内部监听 LV_EVENT_DELETE 自动摘除）；
 * 把长文本模式切到 LV_LABEL_LONG_CLIP 就等于关闭该 label 的跑马灯
 * （本模块只接管处于 CIRCULAR 模式的 label）。
 */

#include "lvgl.h"

/* ==================== 跑马灯节奏（想调就改这里） ==================== */

/** 总滚动轮数（含第一轮）。0 = 无限循环（退回 LVGL 原生行为） */
#define GLOBAL_STYLE_MARQUEE_ROUNDS          3

/** 每轮滚动之间的停顿（ms） */
#define GLOBAL_STYLE_MARQUEE_PAUSE_MS        1000

/** 首次滚动之前的停顿（ms）：先让用户读到标题开头再开始滚，0 = 立即滚动 */
#define GLOBAL_STYLE_MARQUEE_START_DELAY_MS  1000

/** 滚动速度（px/s）。0 = 沿用 LVGL 默认速度（DPI/3，本机 DPI=160 → 约 53px/s） */
#define GLOBAL_STYLE_MARQUEE_SPEED           0

/** 守护定时器周期（ms）：兜底接管 LVGL 重建出来的滚动动画，见文件头 ③ */
#define GLOBAL_STYLE_MARQUEE_POLL_MS         200

/**
 * @brief 把 label 纳入全局跑马灯管理：挂全局动画模板 + 按上面的参数（重新）开始滚动
 *
 * 幂等：同一个 label 可以反复调用（每次都会从头重跑一遍跑马灯）。
 *
 * @param label 必须是 lv_label；其长文本模式须为 LV_LABEL_LONG_SCROLL_CIRCULAR
 *              才会真正开始滚动（CLIP 等其它模式只登记监管、不启动）
 */
void global_style_marquee_attach(lv_obj_t *label);

/**
 * @brief 改写跑马灯 label 的文本并（内容真的变了才）按全局节奏重新开始滚动
 *
 * **改跑马灯 label 的文本请一律用它，不要直接调 lv_label_set_text()。**
 *
 * ⚠ 原因（实测坑）：`lv_label_set_text()` 即使内容**完全相同**也会走
 * `lv_label_refr_text()`，而它在 CIRCULAR 模式下会用模板重建滚动动画
 * （`act_time` 被模板强制成"起始延迟"、`early_apply` 又把偏移拉回 0）——
 * 表现就是"文本没变，跑马灯却莫名回到起点、重新停顿 1 秒"。
 * 列表行尤其敏感：曲目/目录变化时上层会重绑**全部可见行**（只为刷新
 * "正在播放"的黄色高亮），一旦在重绑里裸调 set_text，就会出现
 * "切歌时整屏跑马灯一起重置"。
 *
 * 本函数先比较内容：相同 → 直接返回（动画完全不动），不同 → set_text + 重跑。
 * 所以**真正的换歌依旧是"停顿 1 秒 → 滚 N 轮"**，1 秒初始延迟不受影响。
 */
void global_style_marquee_set_text(lv_obj_t *label, const char *text);
