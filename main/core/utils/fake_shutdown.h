#ifndef FAKE_SHUTDOWN_H
#define FAKE_SHUTDOWN_H

/* ------------------------------------------------------------------ */
/* 伪关机模块：关闭所有外设后进入 deepsleep（近似关机，功耗最低）         */
/*                                                                     */
/* 行为：尽量关闭全部外设（MP3 播放、CS43131 DAC、显示屏、触摸、SD 卡）， */
/* 配置唤醒源后调用 esp_deep_sleep_start() 让芯片进入深度休眠。          */
/*                                                                     */
/* 两种入口：                                                           */
/* 1. fake_shutdown_enter()            普通伪关机（睡眠定时器到点）：     */
/*    关尽外设含显示屏（主动关屏省电），deepsleep，B 键按下唤醒重启。      */
/*    ⚠ 不再由双按触发：双按已改为息屏/亮屏切换（见 core/utils/screen_sleep */
/* 2. fake_shutdown_wait_power_off()   A 键（电源键）长按 2s 触发：     */
/*    配合成品机电源侧"长按 3 秒硬断电"的关机芯片——固件在长按 2s 时     */
/*    保存配置、优雅停播、停外设，屏幕定格提示画面（不主动关屏），       */
/*    随后 deepsleep：用户继续按住 A 键满 3s 即由硬件断电（屏幕靠断电    */
/*    自然熄灭，向用户展示"关机已完成"）；若未断电，按 B 键（GPIO0，     */
/*    RTC GPIO）唤醒重启，即"取消关机"恢复系统。                        */
/*                                                                     */
/* 唤醒源：B 键 GPIO0（RTC GPIO，外部 10k 上拉），按下（低电平）唤醒。   */
/* ⚠ GPIO0 是 strapping 引脚：若唤醒后仍按住会进入下载模式。            */
/* ⚠ A 键 GPIO48 不是 RTC GPIO，无法用于 deep sleep 唤醒（它只负责      */
/*   触发伪关机与硬件断电，断电后无需唤醒）。                            */
/*                                                                     */
/* 注意：deepsleep 唤醒 = 芯片重启，唤醒后会重新走 app_main 初始化，     */
/*       外设（显示/触摸/DAC/SD）在初始化流程中会自动恢复，无需在此恢复。 */
/* ------------------------------------------------------------------ */

/* 伪关机：尽量关闭所有外设（含屏幕）并进入 deepsleep。
   该函数会关闭播放、挂起 LVGL、复位 DAC、关显示/触摸/SD，然后永久休眠；
   若被唤醒，系统整体重启（不会返回）。
   注：关机深链（停外设 + 渲染提示 + deepsleep）委托给 deep_worker 大栈
   任务执行（见 core/utils/deep_worker.h），本函数只做投递并等待完成；
   进入 deepsleep 后芯片睡眠，调用方任务被冻结，函数不再返回。 */
void fake_shutdown_enter(void);

/* A 键（电源键）长按 2s 触发的"等待断电"伪关机：
   保存配置 → 优雅停播 → 屏幕定格提示 → 停外设（不含屏幕）→ deepsleep。
   用户继续按住 A 键满 3s 由关机芯片硬件断电；按 B 键唤醒即取消关机。
   与 fake_shutdown_enter 相同，经 deep_worker 执行；断电或唤醒重启后
   系统恢复。 */
void fake_shutdown_wait_power_off(void);

#endif /* FAKE_SHUTDOWN_H */
