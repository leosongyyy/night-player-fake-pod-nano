# DEV_OPTIONS —— 开发者选项（运行时调试开关）

## 背景

触摸区域测试（白色圆点）原本由 Kconfig（`CONFIG_TOUCH_TEST`）控制，改配置必须
重新构建烧录，不方便日常开关。现改为**运行时开关**：入口在
**设置菜单 → 关于 → 开发者选项**，配置存 NVS（`config_store`），
**开机检查决定是否启动相关任务/创建调试对象**，因此**修改后需重启才生效**
（开发者选项页提供“立即重启生效”按钮）。

> ⚠ S3 时代的“电池电压采样日志”（`battlog` / `battmin`、`battery_logger.c`）
> 已随 AXP2101 适配**整体删除**：S31 有 PMU，电池信息是“随时可读的电压 + 电量计”，
> 不再需要写 SD 卡日志去反推放电曲线。电池自检见 `CONFIG_HW_SELFTEST`
> （Kconfig，非运行时开关，见 `ideas_and_exps/PMU_RTC_DRIVERS.md`）。

## 存储（config_store / NVS，命名空间 "config"）

| 键 | 类型 | 默认 | 含义 |
|---|---|---|---|
| `touchtest` | u8 | 0（关） | 触摸测试白点开关：`lvgl_port_init()` 末尾读取，开则创建白点并订阅触摸 |

相关 API（`core/storage/config_store.h`）：
`config_store_get/set_touch_test(bool)`。setter 只写内存 + NVS、不通知回调
（无 UI 需要实时同步，与 `set_was_playing` 处理一致）。

## 消费点（只在开机初始化路径读取一次）

- `lvgl_port_init()` 末尾（config_store_init 之后）：读 `touchtest`，
  关闭则打 `Touch test dot disabled` 日志；开启时 `touch_test_create()`
  创建 20px 白点跟随手指（抬起 1s 渐变消失）。

## UI（`ui/settings_menu.c`）

- 设置菜单根页 → 关于 → 关于页**最下方**“开发者选项”入口行（⚠ 图标）。
- 开发者选项子页内容：
  1. 触摸测试白点（开关，右侧“开/关”）
  2. 灰色说明行（用途 + “修改保存到 NVS，重启后生效”）
  3. 立即重启生效（小任务延时 300ms 后 `esp_restart`）

## 验证方式

- 首次烧录后默认关闭：日志可见 `config loaded: ... touch_test=0 ...`、
  `Touch test dot disabled`。
- 在开发者选项打开 → 立即重启 → 日志出现 `Touch test dot enabled`。
- release 构建同样内置开发者选项（代码始终编译）；想彻底裁剪仍可走 Kconfig
  移除源码/CMake 条目，但本项目倾向保留（体积影响极小）。

## 相关文件

- `main/core/storage/config_store.h/.c` —— NVS 配置源
- `main/core/ui/touch_test.h/.c`、`main/core/ui/lvgl_port.c` —— 白点创建点
- `main/ui/settings_menu.c` —— 开发者选项页
- `main/Kconfig.projbuild`、`sdkconfig.defaults.*` —— 已移除对应 Kconfig 项
