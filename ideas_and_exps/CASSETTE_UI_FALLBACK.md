# 袖珍磁带机 UI —— 应急回退 Runbook（L1 / L2 / L3）

新 UI **完全替换**旧扁平四页 tabview（`boot_enter_normal()` 一处二选一）。
万一新 UI 黑屏、卡死或触摸失效，按下面三层顺序回退，**不要急着刷机**。

| 层 | 触发条件 | 操作 | 生效时机 | 恢复 |
|---|---|---|---|---|
| L1 组合键 | 新 UI 完全黑屏也能用（不依赖 LVGL） | 开机瞬间**同时按住 A（电源）+ B（GPIO61）约 3 秒** | **toggle** `ui_mode`，本次开机写入、下次启动生效 | 再次开机按 A+B 3s 切回；或在磁带 UI 用 M6 隐藏项切到经典界面 |
| L2 编译开关 | L1 也失效（极端） | `CONFIG_CASSETTE_UI=n` 重新编译烧录 | 永久进旧 UI | 改回 `y` 重编 |
| L3 刷机文档 | 编译环境也不可用 | 按本文命令刷回原厂/上一版固件 | 永久 | 重新烧新固件 |

## L1：开机 A+B 组合键（3s，双向 toggle）

实现位置：`main/main.c` 在 `button_service_init()` 之后调用
`cassette_boot_combo_check()`；实现在 `main/ui/cassette/cassette_fallback.c`。

- **检测口径**：B = `gpio_get_level(GPIO_NUM_61) == 0`（低电平按下）；
  A = `axp2101_is_power_key_pressed()`（PMU IRQ 边沿状态）**或** `0x20
  PWRON_STATUS` bit0（本次开机来源为电源键）。不经过按键回调、不依赖
  LVGL，屏幕全黑也能触发。
- ⚠ **为什么要看 0x20**：从关机态按住 A 开机时，PWRON 按下边沿发生在
  AXP2101 初始化 `axp2101_clear_irq_status()` 之前并被清掉，IRQ 任务收不到
  KEY_DOWN，`axp2101_is_power_key_pressed()` 会是 false。0x20 是 PMU 锁存的
  开机来源，不受清 IRQ 影响，作为"按住电源键开机"的可靠旁证。
- **仅当检测瞬间 B 按住、且 A 满足上述任一条件**才进入 3s 轮询窗口；
  B 中途松开立即退出，正常开机零延迟（串口不会出现 "A+B held at boot"）。
  窗口内 A 的 IRQ 边沿若稍后被捕获同样有效。
- 满 3s **toggle** NVS：命名空间 `cassette_ui`，键 `ui_mode`
  （`0=CASSETTE` / `1=LEGACY`）。日志示例：
  `cas_fallback: combo complete: CASSETTE -> LEGACY`、
  `cas_fallback: ui_mode -> LEGACY (saved)`。
- 下次 `boot_enter_normal()` 读到新值：LEGACY → `app_ui_create()` 走旧
  UI；CASSETTE → 创建 cassette UI。两者都不会同时创建。
- **反向入口**：在磁带 UI 里进 M6「设置」→ 末行低对比隐藏项
  「切换到经典界面（示例）」（`m6_settings.c` 的 `m6_switch_to_legacy_cb`
  ：写 `ui_mode=1` 后 `esp_restart()`）。从经典界面切回用开机 A+B 3s
  （写 `ui_mode=0`）。
- 真机调手感：改 `cassette_fallback.c` 的 `CAS_COMBO_HOLD_MS`（默认 3000）。

> 注意：3 秒是"按住时长"，不要和 PMU 硬件 OFFLEVEL（约 4 秒强制断电）
> 混淆。若按住超过 4 秒且系统正忙，PMU 可能先断电——正常操作按住约 3 秒
> 看到日志/重启进入对应 UI 即可松手。

## L2：Kconfig 编译开关

在 `sdkconfig` 或 `idfc` 配置里把 `CONFIG_CASSETTE_UI` 设为 `n`：

```bash
# 方式 A：直接用 idf.py menuconfig
idf.py menuconfig
#   -> Cassette UI (skeuomorphic 7 modules)
#   -> [ ] 启用袖珍磁带机拟物 UI  ->  关闭

# 方式 B：临时覆盖（不写回 sdkconfig.defaults）
idf.py -D CONFIG_CASSETTE_UI=n build
```

重新编译烧录后，`boot_enter_normal()` 会走 `#else app_ui_create()` 分支，
cassette 源文件即使编进固件也不会被创建。

其它相关开关（均默认安全值）：

- `CONFIG_CASSETTE_FONT_FALLBACK=n`：已烘焙站酷庆科黄油体 + JetBrains Mono
  子集（真等宽），默认走真字体；仅在烘焙产物缺失时才改 `y` 应急；
- `CONFIG_CASSETTE_TAPE_LQ=n`：Flash 紧张时才打开；
- `CONFIG_CASSETTE_LAZY_PAGES=n`：P0 全量常驻，PSRAM 紧张时再评估。

## L3：刷机兜底

```bash
# 完整固件（factory @0x10000）——按本机实际情况替换端口/镜像路径
esptool.py --chip esp32s31 --port /dev/cu.usbserial-XXXX \
    write_flash 0x10000 dist/factory.bin

# 字体分区 0x600000 无需改动：新 UI 的复古子集字库编译进固件，
# 旧的 system.npf 仍被系统字体使用（见 F4 / tools/font/README.md）。
```

⚠ 里世界固件在 `ota_0 @0xC00000`，刷机时不要写错偏移（见
`ideas_and_exps/INNER_WORLD.md`）。

## 验证清单（QA Round 2 真机逐条实测项）

> 说明：三条回退路径的代码均已落地（L1 组合键 `cassette_fallback.c`、
> L2 Kconfig 开关、L3 刷机命令、L1 反向 M6 隐藏项）。静态构建只能证明可
> 编译链接，组合键手感/NVS 持久化/等宽字体必须真机确认。

- [ ] 开机不按组合键：日志无 "A+B held"，正常进磁带 UI，耗时无明显增加；
- [ ] 磁带 UI 下开机按住 A+B 3s：日志出现 `CASSETTE -> LEGACY`，重启进旧四页 UI；
- [ ] 旧 UI 下开机按住 A+B 3s：日志出现 `LEGACY -> CASSETTE`，重启进磁带 UI（双向 toggle）；
- [ ] 旧 UI 下播放/切歌/音量键/通知栏全部正常（底层服务未被破坏）；
- [ ] `CONFIG_CASSETTE_UI=n` 重编：无条件进旧 UI；
- [ ] M6 隐藏项「切换到经典界面（示例）」：写 LEGACY 并重启进旧 UI；
- [ ] A+B 按住 1s 松开（B 先松）：不写 NVS，保持当前 ui_mode；
- [ ] 真字体：M4 翻页钟秒位、M1 `01:24/-02:57`、M7 容量数字横向不抖，
      中文/全角标点无方块（烘焙子集 429 字符，mono fallback 到中文子集）；
- [ ] 状态栏任意位置（标题/时钟/电量/把手）点击一次只开合抽屉一次。
