#ifndef FAKE_SHUTDOWN_H
#define FAKE_SHUTDOWN_H

/* ------------------------------------------------------------------ */
/* 伪关机模块：关闭所有外设后进入 deepsleep（近似关机，功耗最低）         */
/*                                                                     */
/* 行为：尽量关闭全部外设（MP3 播放、CS43131 DAC、显示屏、触摸、SD 卡）， */
/* 配置唤醒源后调用 esp_deep_sleep_start() 让芯片进入深度休眠。          */
/*                                                                     */
/* 唤醒源：B 键 GPIO0（RTC GPIO，外部 10k 上拉），按下（低电平）唤醒。   */
/* ⚠ GPIO0 是 strapping 引脚：若唤醒后仍按住会进入下载模式。            */
/* ⚠ A 键 GPIO48 不是 RTC GPIO，无法用于 deep sleep 唤醒。             */
/*                                                                     */
/* 注意：deepsleep 唤醒 = 芯片重启，唤醒后会重新走 app_main 初始化，     */
/*       外设（显示/触摸/DAC/SD）在初始化流程中会自动恢复，无需在此恢复。 */
/* ------------------------------------------------------------------ */

/* 伪关机：尽量关闭所有外设并进入 deepsleep。
   该函数会关闭播放、挂起 LVGL、复位 DAC、关显示/触摸/SD，然后永久休眠；
   若被唤醒，系统整体重启（不会返回）。 */
void fake_shutdown_enter(void);

#endif /* FAKE_SHUTDOWN_H */
