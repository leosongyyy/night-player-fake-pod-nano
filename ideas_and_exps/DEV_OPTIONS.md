# DEV_OPTIONS —— 开发者选项（运行时调试开关）

## 背景

触摸区域测试（白色圆点）与电池电压采样日志原本由 Kconfig（`CONFIG_TOUCH_TEST` /
`CONFIG_BATTERY_LOGGER_MIN`）控制，改配置必须重新构建烧录，不方便日常开关。
现改为**运行时开关**：入口在 **设置菜单 → 关于 → 开发者选项**，配置存 NVS
（`config_store`），**开机检查决定是否启动相关任务/创建调试对象**，因此
**修改后需重启才生效**（开发者选项页提供"立即重启生效"按钮）。

## 存储（config_store / NVS，命名空间 "config"）

| 键 | 类型 | 默认 | 含义 |
|---|---|---|---|
| `battlog` | u8 | 0（关） | 电池电压日志开关：`battery_logger_init()` 开机读取，开则创建 batt_log 任务 |
| `battmin` | u8 | 5 | 日志采样间隔（分钟）：档位 1/2/3/5/10，UI 循环选择，开机固化 |
| `touchtest` | u8 | 0（关） | 触摸测试白点开关：`lvgl_port_init()` 末尾读取，开则创建白点并订阅触摸 |

相关 API（`core/storage/config_store.h`）：
`config_store_get/set_battery_log(bool)`、`config_store_get/set_battery_log_min(uint8_t)`、
`config_store_get/set_touch_test(bool)`。三个 setter 只写内存 + NVS、不通知回调
（无 UI 需要实时同步，与 `set_was_playing` 处理一致）。

## 消费点（均只在开机初始化路径读取一次）

- `battery_logger_init()`（main.c，sd_card_init 之后）：读 `battlog`/`battmin`，
  关闭则不建任务，并打 `battery logger disabled (developer option off...)` 日志；
  开启时按间隔建 `batt_log` 任务（Core1，4KB 栈，默认 5 分钟一次，
  10 秒窗采样 10 次去极值平均 → 追加写 `/sdcard/battery_log.txt`）。
- `lvgl_port_init()` 末尾（config_store_init 之后）：读 `touchtest`，
  关闭则打 `Touch test dot disabled` 日志；开启时 `touch_test_create()`
  创建 20px 白点跟随手指（抬起 1s 渐变消失）。

## UI（`ui/settings_menu.c`）

- 设置菜单根页 → 关于 → 关于页**最下方**"开发者选项"入口行（⚠ 图标）。
- 开发者选项子页内容：
  1. 电池电压日志（开关，右侧"开/关"）
  2. 日志采样间隔（点击循环 1→2→3→5→10 分钟，右侧"x 分钟"）
  3. 触摸测试白点（开关，右侧"开/关"）
  4. 灰色说明行（两个功能的用途 + "修改保存到 NVS，重启后生效"）
  5. 立即重启生效（小任务延时 300ms 后 `esp_restart`）

## 验证方式

- 首次烧录后默认全关：日志可见 `config loaded: ... batt_log=0(5 min) touch_test=0 ...`、
  `Touch test dot disabled`、`battery logger disabled (developer option off...)`。
- 在开发者选项打开某项 → 立即重启 → 日志出现 `Touch test dot enabled` /
  `battery logger ready: every x min ...`。
- release 构建同样内置开发者选项（代码始终编译）；想彻底裁剪仍可走 Kconfig
  移除源码/CMake 条目，但本项目倾向保留（体积影响极小）。

## 相关文件

- `main/core/storage/config_store.h/.c` —— NVS 配置源
- `main/core/ui/touch_test.h/.c`、`main/core/ui/lvgl_port.c` —— 白点创建点
- `main/core/utils/battery_logger.h/.c` —— 电池日志任务
- `main/ui/settings_menu.c` —— 开发者选项页
- `main/Kconfig.projbuild`、`sdkconfig.defaults.*` —— 已移除对应 Kconfig 项
