## FakePodNanoNightPlayer 项目概览

### 环境说明

**1. 硬件**

本项目是基于 ESP32-S3 成品播放器（开源项目 **FakePodNano** 的硬件）开发的第三方固件。设备通过 USB 串口（COM?，USB-Serial/JTAG 模式）连接，芯片参数：

- 芯片：ESP32-S3 (QFN56) rev v0.2，WiFi/BLE，240MHz 双核
- 存储：8MB Octal PSRAM（AP_3v3）、16MB Flash（QIO，80MHz）
- 屏幕：鱼鹰光电 AMOLED，**460x460**（型号 AM200Q460460LK，QSPI 接口），驱动 `esp_lcd_qspi_amoled`
- 触摸：**CST816 / CST820 / CHSC6417**（按屏幕自动选择），驱动对应 `esp_lcd_touch_cst816` / `esp_lcd_touch_cst820` / `esp_lcd_touch_chsc6417`
- 电源：电池电压经 10K+10K 分压到 GPIO1（ADC1_CH0）
- 耳放芯片：CS43131，I2C控制端口在0x30，通过I2S写入音频

**2. 开发环境**

- ESP-IDF **6.1**（安装于 `C:\esp\v6.1\esp-idf`，GCC 15.2.0），激活脚本 `C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1`。**2026-11 已从 5.5.5 迁移到 6.1，并已在真机逐项验证通过**（播放/音量/伪关机/唤醒/睡眠/息屏/CPU 频率/SD 扫描/无卡/低电压拒启全正常；迁移全过程、两次真机崩溃的根因见 **`ideas_and_exps/IDF6_MIGRATION.md`**，§9.1 是回归清单与"怎么测低电拒启"）；本机同时保留 `C:\esp\v5.5.5\esp-idf` 与对应激活脚本以备回退。这里统一使用idfc进行调用。
  - ⚠ **有一处必要的改动不在本仓库里**：项目支持 exFAT 格式 SD 卡，需 IDF 本体的 FatFs 打开 exFAT —— `C:\esp\v6.1\esp-idf\components\fatfs\src\ffconf.h` 里 `FF_FS_EXFAT` 改为 `1`。**换电脑/重装 IDF 必须重打**，否则 SD 卡挂载失败（`failed to mount card (13)`）并进"无卡"界面（详见 IDF6_MIGRATION.md §6.7）。
  - ⚠ **托管组件有构建期补丁**（`patches/`，由 `main/CMakeLists.txt` 在配置阶段幂等回写；托管组件会被组件管理器覆盖，**必须**这样改）：
    1. `patches/lvgl_fast_mem_decl/` —— 修复 LVGL 把 `LV_ATTRIBUTE_FAST_MEM` 同时放在声明与定义上、与 IDF 6.1 的 `IRAM_ATTR`（带 `__COUNTER__` 的唯一子段）冲突导致的 `-Werror=attributes`（上游 lvgl PR #10737，9.6 仍未合入），补丁只去掉**声明**上的属性、不动定义，故函数仍进 IRAM、零性能损失；
    2. `patches/esp_lcd_touch_deps/` —— 给托管组件 `espressif/esp_lcd_touch` 补显式 `PRIV_REQUIRES esp_driver_gpio`（它调一堆 `gpio_*` 却只写 `REQUIRES "driver" "esp_lcd"`，靠 `esp_lcd` 转发才编过）。
    **属自定义写法，勿当无用代码清理**（详见 IDF6_MIGRATION.md §4.1 与 §9.2）。仓库内 `components/` 下的本地组件**直接改各自的 CMakeLists.txt**，不走补丁。
- LVGL **9.4.0**（managed component，`lvgl/lvgl`；2026-10 由 8.4.0 升级，迁移全过程与踩坑见 `ideas_and_exps/LVGL9_MIGRATION.md`）
- 系统字体 = **font 分区里的 `spiffs/system.ttf`（当前内容 Droid Sans Fallback，AOSP Apache-2.0，50951 字形，3.55MiB）+ FreeType 2.14.3**（`espressif/freetype`；2026-10 由内嵌位图字库迁移，全过程/四个接线坑/栈预算/烧录流程/换字体约定见 **`ideas_and_exps/FontSystem.md`**。⚠ 字体文件**写死**为 `spiffs/system.ttf`，而且**不在固件里**：刷完固件要用 esptool **手动刷一遍字体分区**（一条命令，刷入/校验/换字体流程见 **`ideas_and_exps/FONT_FLASH.md`**），换字体 = 改名覆盖它 + 重刷，不改代码/Kconfig）
- 构建/烧录通过 **`idfc.exe`**（客户端-服务器架构）完成，详见下方"idfc 使用说明"。**下载地址：<https://gitee.com/ZYFDroid/idf-client-cli/releases/>**，把它放到 PATH 中的某个目录（本机为 `C:\Users\ZYFDroid\ProgramFiles\bin\idfc.exe`），随后**在项目目录里直接用 `idfc <子命令>` 调用**（不需要写 `.\idfc.exe`，项目里也不再自带这个 exe）。
- ⚠ **千万不要把 `idfc.exe` 放在本项目工作区内**：DeepSeek Harness v0.1.7-rc.1 起会给工作区打可继承的 **Low 完整性标签**，工作区内的 exe 只能以 Low 启动，服务器的权限自检（写 `%LOCALAPPDATA%\IdfClientCLI`）会失败并拒绝启动，且 **`danger-full-access` 也无法挽救**（它只解除 DSH 沙箱，不解除完整性标签）。放在 PATH 目录里就没有这个问题。

**3. 参考资料**

- `components/`：本地组件（esp_lcd_qspi_amoled AMOLED 驱动、esp_lcd_touch_cst816/820/chsc6417 触摸驱动）。~~i2c_bsp~~ 已于 2026-11 迁移 IDF 6.1 时删除（全工程零引用的死代码）
- `patches/`：**构建期补丁文件**（托管组件用，当前两组：`lvgl_fast_mem_decl/`、`esp_lcd_touch_deps/`，见上方 IDF 6.1 条目）
- `managed_components/`：ESP 组件管理器拉取的依赖（lvgl、esp_lcd_sh8601、esp_lcd_touch、cmake_utilities、chmorgan__esp-libhelix-mp3）
- `IDFC_USAGE.md`（`ideas_and_exps/`）：idfc.exe 的完整使用说明（新版子命令用法、多配置构建原理与历史事故、退出码、常见问题排查）
- `BUILD_CONFIG.md`（`ideas_and_exps/`）：三套构建配置（debug/releasecandidate/release）的完整说明
- `BUILD_MATRIX.md`（`ideas_and_exps/`）：**配置矩阵**（`idfc_buildconfig.json`）的定义、配置文件格式规范、给 idfc 仓库的逐文件修改说明，以及 **2026-10-02 的新版 idfc 验收记录**（§7，含实测证据与已知小问题）

**4. idfc 使用说明（重要）**

`idfc.exe` 是 ESP-IDF 命令行的**服务器 + 客户端**封装：服务器一次性激活 IDF 环境并常驻监听 `http://localhost:19781/idf-client-cli/`（HTTP + SSE 流式输出），客户端把 `idf.py` 命令发给服务器执行。**下载地址：<https://gitee.com/ZYFDroid/idf-client-cli/releases/>**（项目仓库里不再自带该 exe）。

> ⚠ **本项目用的是「子命令版」idfc，旧语法已全部失效**：`--server`、`-B build/<cfg> build`、`-B ... reconfigure` 等写法现在一律报 `没有这样的命令`（退出码 1，不会静默成功）。**历史文档、旧对话记录里的 idfc 命令不要照抄**；完整说明见 `ideas_and_exps/IDFC_USAGE.md`，全部命令与参数用 `idfc help --all` 查。

- **安装（每台开发机一次）**：从上面的发布页下载 `idfc.exe`，解压/复制到 **PATH 中的某个目录**（本机是 `C:\Users\ZYFDroid\ProgramFiles\bin\`）。之后**在项目目录里直接用 `idfc`，不要写路径**（`idfc build`、`idfc status`…）——客户端通过当前工作目录找到 `.idfc/idfc_config.json` 并自动带 `-B build/<cell> …` 参数，不需要 `.\idfc.exe`。验证：`idfc help --all` 有输出即可。
- 🚨 **若 PATH 里找不到 `idfc.exe`（报 `无法将“idfc.exe”项识别为 cmdlet…` / `CommandNotFoundException`，或 `Get-Command idfc.exe` 无结果）**：**不要自己去找、不要拿别处的旧版顶替、也不要改用 `.\idfc.exe`**（工作区里已经没有这个文件了）。此时应**提醒用户下载**，并给出地址 **<https://gitee.com/ZYFDroid/idf-client-cli/releases/>**，请用户把它放到 PATH 中的目录（工作区外），然后重试。
- **首次配置项目**（每个项目目录做一次，写入 `.idfc/idfc_config.json`）：
  ```powershell
  idfc set-idfenv "C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1"   # 版本号按本机安装
  idfc set-variant debug
  idfc ports          # 列出串口（需服务器已启动）
  idfc set-port COMn
  idfc status         # 复核当前生效的构建类型与串口
  ```
- **启动服务器（普通权限即可，不需要管理员）**：
  ```powershell
  idfc start-server                          # 已 set-idfenv 时可省略参数
  idfc start-server --idfenv "<激活脚本路径>"  # 或显式指定本次使用的脚本
  ```
  ⚠ **前提是 `idfc.exe` 位于 PATH 目录而不是项目工作区内**：DeepSeek Harness v0.1.7-rc.1 起会给工作区打可继承的 **Low 完整性标签**（`icacls` 可见 `Mandatory Label\Low Mandatory Level:(OI)(CI)(NW)`），而 Windows 派生进程的完整性取 `min(创建者令牌, 映像文件)`——工作区内的 exe 必定以 Low 启动，写不了 `%LOCALAPPDATA%`（服务器的权限自检正在探测这个），于是启动被拒绝。**`danger-full-access` 也救不了**，它只解除 DSH 沙箱、不解除完整性标签；唯一出路就是把 exe 放到工作区外的 PATH 目录（已实测：普通权限、无提权、无 `danger-full-access` 也能正常启动并长期存活）。
  `start-server` 会轮询服务器状态，**确认就绪后才返回**；服务器控制台就是启动它的那个窗口，**关窗口=停服务器**，停止用 `idfc stop-server`（普通权限即可，它会 `taskkill /t` 自刎）。

- **执行命令**（客户端，在 `workspace-write` 权限下运行，无需提权、无需 `danger-full-access`）：
  ```powershell
  idfc build                                 # 构建（自动检测配置是否过期）
  idfc size                                  # 固件体积分析
  idfc flash                                 # 烧录到已保存的串口
  idfc monitor --wait-for "App started"      # 串口监视，命中后退出
  idfc run --monitor-wait-for "App started"  # 构建+体积+烧录+监视一条龙
  idfc idfpy <任意 idf.py 参数...>             # 逃生舱：原样透传给 idf.py
  ```
- ⚠ 在使用 `idfc idfpy <任意 idf.py 参数...>` 之前，请务必阅读 IDFC_USAGE.md，查看此前旧版本踩过的坑。
- 💡 **执行回显**：`build`/`flash`/`monitor`/`size`（以及 `run` 的各阶段）执行前会打印一行 `# Executing \`idfc idfpy -B build\<cell> -D SDKCONFIG_DEFAULTS="…" -D SDKCONFIG="…" <子命令>\``，即本次**实际使用**的完整命令——想确认"到底在构建哪一套"或手工复现，直接看这一行（启用配置矩阵后是单元格目录，如 `build\cs43131.debug`）。
- ⚠ **`flash` 只警告、不重建**：若 `build/<cell>/sdkconfig` 早于 `sdkconfig.defaults*`、overlay 配置、`idfc_buildconfig.json` 或任何 `Kconfig`/`*.projbuild`，`flash` 会打印"当前构建目标 … 已经过时"的警告，**但仍会把旧产物烧进去**。要烧新配置就得先 `build`（或直接 `run`）。
- 🔎 **查 menuconfig 配置项**（标题 / 说明全文 / 当前值，**纯客户端命令**，不触发构建）：
  ```powershell
  idfc menuconfig search "IRAM SPIRAM"          # 多关键词 AND 检索（每页 20 条，--page N 翻页）
  idfc menuconfig desc CONFIG_BUTTON_LONG_PRESS_MS   # 单项详情（含 help 全文）
  ```
  ⚠ 需要该构建类型**至少成功构建过一次**（`kconfig_menus.json` / `sdkconfig.json` 由 ESP-IDF 在配置阶段生成）；缺失时会提示先 `build` 并退出码 1。没有匹配项同样退出码 1。想改 `main/Kconfig.projbuild` 或调 `sdkconfig` 项之前，先用它确认"这项是干什么的、现在是多少"。
- **⚠ 若提示 `IDF 服务器可能尚未运行（… 请求失败）`（退出码 -1）**：说明服务器未运行或被回收，需要**提示用户手动启动服务器**：在任意目录执行 `idfc start-server`（普通权限即可；也可以让用户开一个终端自己跑）。服务器是常驻进程、且**它的控制台就是启动它的那个窗口，窗口一关就停**，所以每次遇到连接错误都应先检查/提醒启动服务器。
- 未用 `-t` 指定时，构建类型取自 `.idfc/idfc_config.json` 的 `ActiveBuildVariant`（默认为 `debug`）；串口取自 `LastComPort`。**这两个是隐式状态，不确定时先跑 `idfc status`。**

**5. 开发命令**

本项目使用多配置构建系统（debug / releasecandidate / release 三套，详见 `ideas_and_exps/BUILD_CONFIG.md`），以下是不同配置的命令速查表：

** debug（精简字体 + 非极速封面 + 调试全开）** —— 当前项目默认

```powershell
idfc build                      # 增量构建（自动检测配置是否过期）
idfc build --reconfigure        # 强制重新配置（改了 sdkconfig.defaults* 之后）
idfc build --dry-run            # 只看"会做什么"，不实际构建
idfc flash                      # 烧录（串口取自 .idfc 配置）
idfc monitor --wait-for "App started"   # 监视并在命中后退出
```

** releasecandidate（完整字体 + 极速封面 + 调试全开） **

```powershell
idfc build -t releasecandidate
idfc build -t releasecandidate --reconfigure
idfc flash -t releasecandidate -p COMn
```

** release（完整字体 + 极速封面 + 调试全关） **

```powershell
idfc build -t release
idfc build -t release --reconfigure
idfc flash -t release -p COMn
```

**其它常用**

```powershell
idfc run -t debug --monitor-wait-for "App started"   # 构建+体积+烧录+监视一条龙
idfc size -t debug                                   # 固件体积分析
idfc clean -t debug        # 删当前单元格的 sdkconfig 与 config（如 build/cs43131.ttffont.debug/…）
idfc fullclean -t debug    # 删当前单元格整个目录（如 build/cs43131.ttffont.debug）
idfc help --all            # 全部命令与参数
```

**备注**：

- **过期检测已自动化**：改了 `sdkconfig.defaults*`、任何 overlay 配置（`sdkconfig.<维度>.<类型>`）、`idfc_buildconfig.json` 或 `main/Kconfig.projbuild` 后，`build` 会自动发现并清理+重新配置，**不需要再手工删 `build/<cell>/sdkconfig`**。想强制全量重配用 `build --reconfigure`，想跳过检测走纯增量用 `build --no-reconfigure`，想从最干净状态开始用 `build --fullclean`。
- ⚠ 原理仍值得知道（历史事故的根因）：**已存在的 `build/<cell>/sdkconfig` 优先级高于 defaults**（`replace=False` 合并），所以只跑普通增量构建时新配置**不会生效、也不会报错**。历史上曾因此产出"堆毒化仍开"的固件（内部 RAM 紧张、任务建不起来、SD 读 DMA 失败）。**手工绕过 `build` 直接用 `idfpy -B ...` 时必须自己注意这一点。**
- 串口优先级：`-p` > `.idfc` 里 `set-port` 保存的值。构建类型优先级：`-t` > `.idfc` 里 `set-variant` 保存的值（当前 `debug`）；overlay 维度优先级：`-o <维度>=<类型>` > `.idfc` 里 `set-overlay` 保存的 `ActiveOverlays` > 矩阵定义里的 `DefaultType`（当前 `dac_chip=cs43131`、`font=ttffont`）。
- 默认构建idfc当前默认构建类型，除非用户指定。
- `idfc -B build/<cfg> build` 之类的旧写法已全部失效（见第 4 节）。
- 已开放 `C:\esp\` 与 `C:\Espressif\` 的读取权限，可查阅 IDF 源码。

**6. 构建配置**

- `main/Kconfig.projbuild`：应用配置项（menuconfig）。配置项目请阅读该文件获得配置项的详细信息。常用配置组合均已使用三套构建配置切换。
- `sdkconfig.defaults`（+ `sdkconfig.defaults.debug` / `.releasecandidate` / `.release`）：esp32s3、QIO 16MB Flash、自定义分区表、8MB Octal PSRAM（含 XIP 指令/只读数据）、240MHz、64B 数据缓存线、FreeRTOS 1000Hz、**LVGL 9：`CONFIG_LV_DEF_REFR_PERIOD=33`（显示刷新与 indev 轮询在 v9 合并为这一个周期）、`CONFIG_LV_USE_CUSTOM_MALLOC=y`（LVGL 对象堆走 `core/ui/lvgl_mem.c` 进 PSRAM，见"关键技术要点"；该文件另有"热点对象窗口"接口，把两个滚动列表页的对象钉回内部 RAM）、`CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=y`（blend 留在 IRAM；关掉可再省 83KB 内部 RAM、代价约 2fps）、裁掉 6 种用不到的颜色格式（`XRGB8888` / `ARGB8888_PREMULTIPLIED` / `L8` / `AL88` / `A8` / `I1`，保留 `RGB565`+`ARGB8888`+`RGB565A8`+`RGB888`；LVGL 的 IRAM 占用 82.8→25.3KB）、关 `CONFIG_LV_BUILD_EXAMPLES` / `LV_BUILD_DEMOS`（examples 下 250 个 .c 编了也会被 `--gc-sections` 全丢，纯省全量编译时间）、`CONFIG_LV_USE_CLIB_STRING=y`、`CONFIG_LV_USE_SYSMON=y`+`CONFIG_LV_USE_PERF_MONITOR=y`（v9 起性能监视器挂在 SYSMON 下，SYSMON 默认 n，不显式打开会被 Kconfig 静默丢弃）、字体 12/16/20/24/28/32/48 + 默认 18**、开启 FreeRTOS 运行时统计（`GENERATE_RUN_TIME_STATS`，performance_monitor 依赖）、FATFS codepage 936 + UTF-8、`CONFIG_UI_COVER_FLASH=y`（公共默认即极速版封面）、**字体由 `font` 维度决定 + 默认 `CONFIG_UI_FONT_TTF=y`：`CONFIG_LV_USE_FREETYPE=y` + `CONFIG_LV_FREETYPE_USE_LVGL_PORT=y` + `CONFIG_LV_FREETYPE_CACHE_FT_GLYPH_CNT=1024`（同时管度量缓存 ×2 与位图缓存 ×1，只吃 PSRAM；2026-10 由 256 上调）—— 这一组就是 `sdkconfig.font.ttffont` 的内容，选 `binfont` 时被 `sdkconfig.font.binfont` 整体覆盖（关 TTF、关 FreeType、开 `UI_SYSTEM_FONT_FULL`），见下方"配置矩阵"。系统字体走 font 分区的 `spiffs/system.ttf`（当前内容 Droid Sans Fallback，**字体文件写死为 `spiffs/system.ttf` 且在固件之外——刷完固件要用 esptool 手动刷入**、没有 Kconfig 选项）**、`CONFIG_ESP_MAIN_TASK_STACK_SIZE=16384`（TTF 迁移时由 8192 上调，见 `FontSystem.md` §5）等
  - ⚠ v8 的 `LV_COLOR_16_SWAP` / `LV_COLOR_SCREEN_TRANSP` / `LV_DISP_DEF_REFR_PERIOD` / `LV_INDEV_DEF_READ_PERIOD` / `LV_USE_LARGE_COORD` 在 v9 已删除或改名，逐项对照表见 `ideas_and_exps/LVGL9_MIGRATION.md` §2
- **版本管理：配置事实源只有 `sdkconfig.defaults*`、`sdkconfig.<维度>.<类型>`（overlay 配置）、`idfc_buildconfig.json` 与 `main/Kconfig.projbuild`（均纳入 git）**；生成的根 `sdkconfig` 与各 `build/<cell>/sdkconfig` **已被 `.gitignore` 忽略，不入库**。多配置构建下每个单元格目录持有自己的 sdkconfig（`-D SDKCONFIG=build/<cell>/sdkconfig` 指定）；`fullclean` 只清当前单元格目录。改配置的正确姿势：改 `sdkconfig.defaults*` / overlay 配置 / `idfc_buildconfig.json` / `Kconfig.projbuild` 后执行 `idfc build`（会自动检测过期并清理+重新配置），或 `idfc build --reconfigure` 强制重配。
- **配置矩阵（`idfc_buildconfig.json`）—— 已在新版 idfc 中实现并验收通过（2026-10-02）**：除 debug/releasecandidate/release 三个 variant 外，还有**两个 overlay 维度**：
  - **`dac_chip`**（`cs43131` 默认 / `pcm5102`）：目前**只影响启动日志**（`main/main.c` 末尾用 `#if` 打印 `DAC chip: CS43131/PCM5102`），配合 `main/Kconfig.projbuild` 的 `AUDIO_DAC_CS43131` / `AUDIO_DAC_PCM5102`（两个独立 bool，必须且只能开一个，同时开/同时关都 `#error`）；公共默认在 `sdkconfig.defaults` 选 `AUDIO_DAC_CS43131=y`。真正的 DAC 切换尚未实现。
  - **`font`**（`ttffont` 默认 / `binfont`，2026-10-25 新增）：**真切换**。`ttffont` = font 分区里的 `spiffs/system.ttf` + FreeType（`UI_FONT_TTF=y`、`LV_USE_FREETYPE=y`）；`binfont` = 完整版内嵌位图字库整份编进固件（`UI_FONT_TTF` 关、`LV_USE_FREETYPE` 关、`UI_SYSTEM_FONT_FULL=y`），完全不用 FreeType、也不用 font 分区（也就不需要手动刷字体）。实测 debug 固件 **ttffont 1.85 MiB / binfont 3.84 MiB**；⚠ binfont 的字库 `.rodata` 会被 XIP 拷进 PSRAM，可用 PSRAM 由 6.40MB 降到 4.35MB——这是**内嵌字库方案的固有特性**（TTF 迁移前的老方案同样如此，属已知行为），不是引入 font 维度才有的问题。
  ⚠ **构建目录是 `build/<dac_chip>.<font>.<variant>`**（如 `build/cs43131.ttffont.debug`）：`OutputDir` 必须含**全部**维度占位符，所以 2026-10-25 加 `font` 维度后所有 cell 目录都改了名；旧的 `build/<variant>`、`build/<dac_chip>.<variant>` 目录**不再被复用**（首次会从零全量构建；binfont 因为要编译那份 16MB 的字库源文件，实测 167 秒），可手工删除。常用命令：`-o font=binfont` 临时指定、`idfc set-overlay font binfont`（`--default` 清除）永久保存、`idfc matrix` 查看全部 **12 个单元格**与当前选择、`idfc status` 看当前单元格。overlay 配置命名约定为 `sdkconfig.<维度>.<类型>`（`sdkconfig.dac_chip.*` / `sdkconfig.font.ttffont` / `sdkconfig.font.binfont`），在 BaseConfig 与 variant 之后按 `Overlays[]` 声明顺序叠加。详见 `ideas_and_exps/BUILD_CONFIG.md` §8 与 `ideas_and_exps/BUILD_MATRIX.md`（定义/格式/修改说明 + §1.7 当前矩阵 + §1.8 font 维度 + §7 验收记录）。
- `partitions.csv`（**显式静态布局**，偏移写死不随自动分配漂移）：bootloader@0x0 / 分区表@0x8000（两者由 sdkconfig 默认 `BOOTLOADER_OFFSET_IN_FLASH`、`PARTITION_TABLE_OFFSET` 控制，不在 CSV 内）→ `nvs`@0x9000(0x6000) / `phy_init`@0xf000(0x1000) / `factory`(app，**0x7F0000=7.9375M**)@0x10000 / **`font`(data 0x41，4M)@0x800000**（系统字体 `system.ttf`，当前内容 Droid Sans Fallback（AOSP Apache-2.0，50951 字形，3.55MiB），**裸二进制、无文件系统**）/ `pldata`(data 0x40，4M)@**0xC00000**（播放列表数据结构分区，内存映射只读访问，**锚定在分区表最末尾**）。⚠ **三条布局原则**：① **`pldata` 必须待在最后** —— 它是"整体重写 + 头部 commit"的只读 mmap 数据，偏移一变旧播放列表就作废、首次开机必须重扫；锚定尾部后调整 `font`/`factory` 只动它俩之间那条线，`pldata` 再也不动；② `font`/`pldata` 偏移都要按 64KB（`SPI_FLASH_MMU_PAGE_SIZE`）对齐才能整体内存映射；③ `factory` 起始固定 0x10000（app 对齐要求），与 `font` 平分中间那块空间。2026-10 重排过**两次**：第一次（字体 TTF 化，原 factory 6M、pldata@0x610000）字体 7.82MiB 装不进尾部剩余空间，从 factory 割 2MiB 给 font；第二次（字体换成 3.55MiB 的 Droid 后）font 8M→4M、factory 3.9375M→7.9375M，并把 pldata 从 0x400000 挪到尾部 0xC00000 锚定（**font 起点仍是 0x800000 → 字体数据不受影响、不用重烧**）。⚠ **两次都改过 pldata 偏移 → 旧播放列表数据作废、首次开机要走一次"重新扫描"**（nvs 未动，配置不丢）。选型与实测见 `ideas_and_exps/FontSystem.md` §2。
- **字体与构建类型（debug/release*）解耦了**：字体由 `font` 维度决定（见上"配置矩阵"），调试版与发布版用的是同一份字体，不再有"调试版字体不全"的说法。换字体 = 用新字体文件**改名覆盖** `spiffs/system.ttf` → `idfc build` + **手动重刷字体分区**（`esptool.exe --chip esp32s3 -p COMx write_flash 0x800000 spiffs/system.ttf`，详见 `ideas_and_exps/FONT_FLASH.md`）；同时记得更新内嵌位图回退字库的字形清单（`ideas_and_exps/FontSymbols.md`）。

### 项目内容

**FakePodNanoNightPlayer —— FakePodNano 播放器的第三方固件**

> 本项目目标：为成品开源播放器 **FakePodNano**（ESP32-S3 + AMOLED + CS43131 + 触摸）开发**更好用、专注于音乐播放**的第三方固件，替代/改进原厂固件的使用体验。

**项目现状**：核心音乐播放体验已**基本完整**——本地 MP3 按文件夹/歌单浏览、四页滑动 UI、全屏封面正在播放页、下拉控制中心、睡眠定时、EQ、随机播放、记忆播放、自动关机保护均已实现。剩余功能/规划见 `ideas_and_exps/FUTURE_PLAN.md`（可能实现也可能被砍掉）。

> 关于 `FUTURE_PLAN.md` 和 `DONE_PLAN.md`: 这两个文件主要存储较大型的需求和修复内容，以及其完成情况。日常小需求和调整无需记录在内。只有在 FUTURE_PLAN.md 中出现过的内容才可以移动到 DONE_PLAN.md 中。若在进行小需求时踩到了坑导致变成了大型研究，建议单开一个文件进行记录（例如 IRAM_DMA_AUDIT.md）

**当前实现状态：**
- 播放核心：本地 MP3 播放（Helix 解码，实测 5000+ 首大曲库），按**文件夹**组织、支持 **`.pl` 文本歌单**；**循环模式**（全部/列表/单曲）+ **顺序/随机**；**无限确定性随机播放**（分块 + 确定性洗牌，断点续播）；回放增益；**EQ 均衡器**（10 段浮点 PeakEQ + 预增益，NightPlayer 兼容 `.eq` 文件）；**封面/歌曲信息**（`.npd` 元数据 + JPEG 封面解码）
- 硬件层：AMOLED 显示（QSPI，支持 6 种屏幕尺寸配置切换，**全黑启动防开机白屏闪烁**）、触摸（CST816/820/CHSC6417 按屏幕自动选择，**异步初始化不阻塞开机**）、电池电压 ADC、音频（I2S 主机 + CS43131 DAC + 回放增益 + EQ）、SD 卡（SDMMC 4-bit + FAT/exFAT，挂载 `/sdcard`；⚠ exFAT 需给 IDF 本体的 FatFs 打补丁，见"开发环境"）
- **任务核心布局：UI（LVGL）固定 Core0、播放解码（MP3/封面）固定 Core1，二者分核互不抢占（见使用技术 → 2. 任务核心布局）**
- LVGL 移植层：显示驱动、2ms tick、专用 LVGL 任务（固定 Core0）、互斥锁、触摸 indev（33ms，x/y 坐标 ×29/30 校准）、触摸事件订阅回调、辅助 UI 任务注册（随 LVGL 挂起/恢复）、**通用虚拟列表组件**（virtual_list）、**全局跑马灯节奏**（global_style：起始延迟 + 滚动 N 轮 + 轮间停顿，见 MARQUEE_STYLE.md）；息屏/伪关机的深调用链统一委托 deep_worker 大栈任务（见第 6 节）
- 开机流程（boot_flow）：**开机封面（默认封面，LVGL 提前启动后立刻点亮，见使用技术 → 1）** → 低电拒启（<3.45V 停充电警告屏）→ 未插卡提示 → 播放列表数据无效则一键扫描（实时进度）→ 正常进入主界面；音乐变动后可在设置菜单"重新扫描"。**设置菜单不参与开机**（懒加载，首次点开设置才构建）
- UI：主界面（四页滑动 tab：目录/歌单列表 → 歌曲列表 → 正在播放 → 播放设置）、顶部状态栏（左=音量档位、中=当前页面名、右=电池电压）、下拉控制中心（电池/亮度竖条/音量竖条/睡眠定时开关/设置入口，**纯 LVGL 事件驱动**拖拽）
- 工具：双核 CPU/内存性能监视（串口每 2s 输出，Kconfig）、**开发者选项**（设置菜单 → 关于 → 开发者选项：电池电压日志开关/采样间隔、触摸区域测试白点开关，NVS 运行时配置**默认关**，开机检查、修改需重启生效）、全局 Toast 提示

### 代码结构

```
main/
  main.c                    # app_main：config_store → display → LVGL 移植层 + 开机封面（提前点亮）→ 其余硬件 → boot_flow → 深链/睡眠/按键/电源/日志/性能监视
  CMakeLists.txt
  Kconfig.projbuild         # 构建级应用配置项（Kconfig；触摸测试/电池日志已移至运行时开关）
  core/
    hardware/               # 与应用无关的底层硬件驱动
      display.h/.c          # AMOLED LCD（QSPI 总线/面板）+ 屏幕尺寸配置 + 亮度控制
      touch.h/.c            # 触摸控制器（I2C + CST816/820/CHSC6417 按需选择，异步初始化）
      battery.h/.c          # 电池电压 ADC（分压换算 + 校准；BATTERY_CRITICAL_LOW_MV=3450）
      button_service.h/.c   # A/B 双键服务（GPIO48/GPIO0，去抖 + 单击/双按/长按事件）
      sd_card.h/.c          # SD 卡驱动（SDMMC 4-bit + FAT/exFAT 挂载 /sdcard）
      pldata.h/.c           # 播放列表数据结构分区（4MB）驱动：esp_partition_mmap 内存映射只读
      audio/
        audio.h/.c          # 音频播放 API（I2S 主机 + CS43131 DAC + PCM 流写 + 回放增益 + 统一浮点路径）
        cs43131.h/.c        # CS43131 DAC 驱动（I2C 控制、24.576MHz 直通 MCLK、ASP 从机 I2S、音量）
        mp3_player.h/.c     # MP3 解码播放（Helix + 跳 ID3v2 + 写 I2S，Core1 后台任务）
        eq.h/.c             # EQ 均衡器（10 段浮点 PeakEQ + 预增益，.eq 文件解析/NVS 持久化/DSP 线程安全）
    player/                 # 播放引擎（应用层）
      player_controller.h/.c  # 播放控制器（Core0 独立后台服务：位置维护/播放控制/循环/随机/坏曲事件）
      shuffle_player.h/.c     # 无限确定性随机播放列表（分块 + 确定性洗牌）
    storage/                # 播放器数据持久化
      config_store.h/.c     # 用户配置存储（NVS：音量/亮度/循环/随机/睡眠/息屏超时/EQ/上次位置等）
      playlist.h/.c         # 播放列表只读解析（pldata 内存映射 → 目录/歌曲/字符串）
      playlist_store.h/.c   # 播放列表存储（pldata 分区"整体重写 + 头部 commit"语义）
      playlist_builder.h/.c # 播放列表构建器（SD 递归扫描 → PLST 格式 → 写 pldata）
      playlist_format.h     # PLST 二进制数据格式（目录区/歌曲区/字符串池，固定布局）
    music/
      song_meta.h/.c        # 歌曲元数据（.npd 文件解析：标题/艺术家-专辑/回放增益/封面）
    image/
      cover_jpeg.h/.c       # JPEG 封面解码（esp_jpeg/tjpgd → LVGL RGB565 img，后台任务）
      default_cover.h/.c    # 默认封面（2bit 索引色，省 Flash）
      default_cover_flash.c # 默认封面（RGB565 极速版，423KB，CONFIG_UI_COVER_FLASH）
    fonts/
      system_font.h         # 系统字体统一引用 UI_SYSTEM_FONT（随 CONFIG_UI_SYSTEM_FONT_FULL 切换）
      system_global_font.h/.c    # 完整版中文字库（约 2.4MB）
      system_global_font_lite.h/.c # 精简版（仅系统 UI 用字 + ASCII）
      system_smaller_font.h/.c   # 状态栏小字体
    ui/                     # 通用 UI 基础设施（与应用无关）
      lvgl_port.h/.c        # LVGL 移植层：显示驱动、tick、任务(Core0)、互斥锁、触摸 indev、触摸事件订阅
      lvgl_mem.h/.c         # ⭐ LVGL 9 自定义分配器：把 LVGL 的对象/样式/绘制层堆整体放到 PSRAM（内部 RAM 留给 DMA 绘制缓冲），见 LVGL9_MIGRATION.md §10；另有"热点对象窗口"接口 lvgl_mem_set_internal_prefer()——把两个滚动列表页的热对象钉回内部 RAM（调用点 main_ui.c / virtual_list.c），见 IRAM_AUDIT_LVGL9.md §13
      touch_test.h/.c       # 触摸区域测试（白点跟随 + 淡出；运行时开关，开发者选项控制）
      virtual_list.h/.c     # 通用虚拟滚动列表（固定行槽复用，600+ 长列表可靠；v9 的坐标为 int32_t，不再需要 LARGE_COORD）
      click_wheel.h/.c      # 虚拟 ClickWheel 速滑按钮（屏幕层黄色圆钮，按住绕屏中心画圈翻页 + 12 灰点指示；⚠ 不可挂 lv_layer_top——会让被下拉控制中心遮住的页面内容每帧重绘、严重掉帧，见 CLICK_WHEEL_USAGE.md 与 DONE_PLANS #38；滚完整个列表的圈数由条目数按对数模型 y=log_1.618(x)-7 决定；NVS 三项配置 click_wheel / clkwheel_rev / clkwheel_spd（默认开/正常/普通），设置菜单 →「虚拟 ClickWheel 选项」子页（开启/滑动方向/速度：普通×1.00、快速×0.75、极速×0.50 点击循环）可调，均立即生效——以上全部见 DONE_PLANS #42）
      global_style.h/.c     # 全局 UI 风格/动画统一样式（跑马灯：起始延迟 + 滚动 N 轮 + 轮间停顿，参数在头文件宏里，**不进 Kconfig**；v9 的 lv_label 已原生认动画模板里的轮数，只需维护模板 + lv_obj_set_style_anim；⚠ 改跑马灯文本必须用 global_style_marquee_set_text()——裸 lv_label_set_text 即使内容相同也会重建动画，会表现为"重绑列表时跑马灯全被打回起点"，见 MARQUEE_STYLE.md 与 DONE_PLANS #40）
    utils/
      performance_monitor.h/.c  # 双核 CPU/内存监视（从 OtoClock 移植，含 ISR 采样 CPU 频率）
      fake_shutdown.h/.c        # 伪关机（睡眠定时器到点；A 键长按"等待断电"，深链委托 deep_worker）
      sleep_timer.h/.c          # 睡眠定时器（到点音量渐降后触发普通伪关机；清醒感知加时）
      power_service.h/.c        # 电源服务（原 screen_sleep 改名）：息屏/亮屏 + 无操作自动息屏 + 自动关机策略
      battery_logger.h/.c       # 电池电压采样日志（续航测量用；开发者选项运行时控制）
      deep_worker.h/.c          # 深路径执行器（大栈任务）：息屏/亮屏/伪关机深调用链唯一执行者
      task_psram.h/.c           # PSRAM 栈任务创建助手（task_create_psram_stack，失败自动回退内部 RAM）
  ui/                       # 应用 UI
    app.h/.c                # UI 组装入口 + 顶部状态栏（左音量/中页面名/右电池电压）
    boot_flow.h/.c          # 开机流程控制器（低电拒启/无卡/扫描/进主界面）
    main_ui.h/.c            # 主界面（四页滑动 tab：目录/歌曲/正在播放/播放设置）
    dir_list.h/.c           # 目录/歌单列表页（virtual_list，/sdcard 头部 + 行图标）
    playlist_page.h/.c      # 歌曲列表页（virtual_list，点击播放，当前曲目高亮）
    now_playing.h/.c        # 正在播放页（全屏封面 + 标题栏 + 控制栏/进度条，点击封面隐藏呼出）
    settings_page.h/.c      # 播放设置页（九宫格：循环/随机/睡眠定时/EQ/回放增益开关）
    settings_menu.h/.c      # 全屏设置菜单（睡眠/息屏/按键帮助/关于+开发者选项/重新扫描）；**分级懒加载**：外壳+根页面首次打开时建（44ms），子页首次进入才建，不占开机路径（见 DONE_PLANS #44）
    notification_panel.h/.c # 下拉控制中心（电池/亮度/音量竖条/睡眠开关/设置入口，LVGL 事件驱动）
    key_handler.h/.c        # 按键分发（亮屏：音量±/伪关机/息屏；息屏：播放控制/亮屏）
    player_ticker.h/.c      # 播放器状态轮询刷新 + 坏曲事件 toast 拉取
    eq_popup.h/.c           # EQ 预设选择弹窗（列出 .musicfx 下 .eq 文件）
    toast.h/.c              # 全局 Toast 提示（底部弹出 + 环形倒计时，顶层透传）
    list_common.h/.c        # 列表页公共小工具
components/
  esp_lcd_qspi_amoled/      # AMOLED 面板驱动（QSPI，含亮度 0x51 命令）
  esp_lcd_touch_cst816/     # CST816 触摸
  esp_lcd_touch_cst820/     # CST820 触摸
  esp_lcd_touch_chsc6417/   # CHSC6417 触摸
patches/
  lvgl_fast_mem_decl/       # LVGL 构建期补丁（上游 PR #10737），由 main/CMakeLists.txt 回写
  esp_lcd_touch_deps/       # 托管组件 esp_lcd_touch 的依赖补丁（补 esp_driver_gpio）
```

### GPIO 分配（随项目进度持续发掘）

> ⚠ **重要**：本项目的 GPIO 用法是**随开发进度逐渐发掘和补充**的。当前已确认表中所列；其余（麦克风、IMU 等）在源码历史注释中有规划（见下方"历史注释规划"），**尚未验证启用**。每次新增外设功能时，先核对本表避免冲突，确认用法后更新本表。

**当前已确认使用的 GPIO：**

| GPIO | 功能 | 说明 |
|---|---|---|
| 0 | B 键 | 按键接 GND，外部 10kΩ 上拉（`core/hardware/button_service.c`）。⚠ strapping 引脚：复位时若被按住会进入下载模式。RTC GPIO，deepsleep 唤醒源：伪关机后按下可"取消关机"并唤醒重启 |
| 1 | 电池电压 ADC | ADC1_CH0，10K+10K 分压（`battery.c`） |
| 2 | 触摸 SCL | I²C 时钟（`touch.h`） |
| 3 | 触摸 SDA | I²C 数据（`touch.h`） |
| 4 | 触摸 INT | 触摸中断（`touch.h`） |
| 5 | 触摸 RST | 触摸复位（`touch.h`） |
| 7 | LCD RST | 屏幕复位（`display.h`） |
| 8 | LCD CS | QSPI 片选（`display.h`） |
| 9 | LCD PCLK | QSPI 时钟（`display.h`） |
| 10~13 | LCD DATA0~3 | QSPI 数据线（`display.h`） |
| 14 | SD DAT1 | SDMMC 数据线 1，10k 上拉（`core/hardware/sd_card.c`） |
| 15 | SD DAT0 | SDMMC 数据线 0，10k 上拉（`core/hardware/sd_card.c`） |
| 16 | SD CLK | SDMMC 时钟，直连 MCU（`core/hardware/sd_card.c`） |
| 17 | SD CMD | SDMMC 命令线，10k 上拉（`core/hardware/sd_card.c`） |
| 18 | SD DAT3 | SDMMC 数据线 3，10k 上拉（`core/hardware/sd_card.c`） |
| 21 | SD DAT2 | SDMMC 数据线 2，10k 上拉（`core/hardware/sd_card.c`） |
| 38 | 音频 LRCK/WS | I2S 帧时钟，主机输出（`core/hardware/audio/audio.c`） |
| 39 | 音频 DIN | I2S 数据输出（`core/hardware/audio/audio.c`） |
| 40 | 音频 BCK | I2S 位时钟，主机输出（`core/hardware/audio/audio.c`） |
| 41 | CS43131 RESET# | DAC 复位，低有效，经 3.3→1.8V 电平转换（`core/hardware/audio/cs43131.c`） |
| 48 | A 键（电源键） | 按键接 GND，内部上拉（`core/hardware/button_service.c`）。⚠ 与电源侧**3 秒关机芯片复用同一按键**：长按满 **1.2s** 固件伪关机（`fake_shutdown_wait_power_off`，保存配置+停外设+定格提示+deepsleep；阈值 `CONFIG_BUTTON_LONG_PRESS_MS`），继续按住满 3s 由硬件直接断电/开机。非 RTC GPIO，无法用于 deepsleep 唤醒 |

**历史注释规划（源码 `main.c` 旧版注释，当前未启用、未验证）：**
- LCD 相关：`TFT_TE = 6`（撕裂效果同步）
- I²C 总线：`I2C_WIRE_SDA = 3`、`I2C_WIRE_SCL = 2`（与触摸共用）
- 音频（I2S + CS43131 DAC）：`MCK=-1, BCK=40, WS=38, DO=39, CS43131_RST=41`（**已确认启用**，见 `core/hardware/audio/`；MCLK 由板载 24.576MHz 晶振提供，不占用 GPIO）
- 按键/静音：~~普通版 `BTN_PIN=47`、`AUDIO_MUTE=48`；HIFI 版 `BTN_PIN=48`~~（**已确认**：A 键=GPIO48、B 键=GPIO0，见 `button_service.c`）
- 麦克风（PDM）：`MIC_PDM_DATA=46, MIC_PDM_SCK=45`
- IMU 中断：`IMU_INT1=47`（HIFI 版）

**已确认的 I²C 设备（开机扫描）：** 0x15、0x30（CS43131）、0x6b、0x7e。

### 使用技术（架构总览）

> 各模块的接口、内部实现与踩坑细节均在对应源码头文件/源文件注释中，读代码时可自然了解；本部分只保留跨模块的架构与通用约定。

**1. 分层架构**
- `core/` 放与应用无关的底层：`core/hardware/`（显示/触摸/电池驱动）、`core/ui/`（LVGL 移植层 + LVGL 分配器 lvgl_mem + 触摸测试 + 虚拟列表 + ClickWheel 速滑按钮 + 全局跑马灯节奏 global_style）、`core/utils/`（性能监视/电源/深链）；`core/player`、`core/storage`、`core/music`、`core/image`、`core/fonts` 放播放引擎与数据层；`ui/` 放应用 UI（app、main_ui、boot_flow、notification_panel、settings_menu 等）
- `main.c` 只做初始化装配：`config_store_init → deep_worker_init → display_init → lvgl_port_init → boot_splash_show(持锁首帧+点亮默认封面) → touch_init → battery_init → audio_init → audio_set_volume_level → EQ 恢复 → pldata_init → button_service_init → sd_card_init → boot_flow_run(持锁切屏+复核亮度) → sleep_timer_init → key_handler_init → power_service_init → battery_logger_init → startPerformanceMonitor()`
  - ⚠ **LVGL 故意提前到 display 之后立刻启动**（唯一硬依赖是面板句柄 + config_store，触摸句柄可传 NULL 由异步回调补上）：这样能马上画一张默认封面并点亮，把 touch/audio/pldata/SD 与 `app_ui_create`（实测约 700ms）的耗时藏在封面背后——实测首次点亮从约 1.9s 提前到约 0.7s，进主界面的时刻不变。开机封面由 `boot_splash_show()`（`ui/boot_flow.c`）创建，随后 `boot_flow_run()` 的任一分支都会删除它并替换为对应界面；防白屏顺序（亮度 0 时先写满整屏 → 再置亮度）与亮度必须与刷屏同锁的约定均未变。

**2. 任务核心布局（UI Core0 / 音频 Core1）**
- 动机：息屏时 UI 完全静止（挂起），把 **UI（LVGL）固定在 Core0**、**播放解码/封面解码固定在 Core1**，二者分核互不抢占。
- **深链执行器 deep_worker**（`core/utils/deep_worker.h/.c`：常驻任务，栈 **12KB**（2026-10 TTF 迁移由 8KB 上调：息屏唤醒的 `lv_refr_now` 全屏重刷会光栅化字形，见 `FontSystem.md` §5）、prio 6、Core0，信号量驱动）：息屏/亮屏（esp_pm_configure + AMOLED 重初始化 + lv_refr_now 全屏重刷）、伪关机（停外设 + 提示渲染 + deepsleep）、以及**需要内部栈的关缓存段**（如"重新扫描"的 playlist munmap/擦头）统一委托它串行执行；业务任务只需 `deep_worker_call(fn, arg)` 投递并等待完成。⚠ 约定：fn 内禁止再 deep_worker_call；调用方禁止持 LVGL 锁投递；勿回到"在小任务里直接跑深链"（历史爆栈记录）。⚠ **栈 12KB 且必须在 `app_main` 早期初始化**（config_store 之后、硬件初始化之前）：内部 RAM 开机末期碎片化（最大连续块约 8.6KB），放到 boot_flow 之后再创建会失败；历史上曾因把这种大栈任务挪到开机末期 + 未检查 `xTaskCreatePinnedToCore` 返回值而**静默失败** → 任务不存在 → 每次深调用等满 5s 超时（按键息屏/亮屏延迟 5 秒）。现已检查返回值并加 `s_exec_task == NULL` 快速失败（就地兜底执行）+ 超时时 dump 全部任务状态。⚠ **该"早期创建"约束也正是 12KB 能分配成功的前提**（早期内部 RAM 还有 ~200KB 空闲/118KB 连续；实测创建成功、HWM 余 11.5KB）。
- Core0：`LVGL`（lvgl_port_task）、`button_svc`、`deep_worker`、`player_ctrl`、`cpu_coll0` 及系统 esp_timer 任务/ISR（sdkconfig 固定 CPU0）与全部驱动中断（app_main 于 Core0 初始化）。
- Core1：`mp3_play`/`mp3_out`（解码输出，播放时建/常驻）、`cover_load`（封面解码，注册为 lvgl_port 辅助任务息屏随 LVGL 挂起）、`sleep_timer`、`power_svc`、`batt_log`（电池电压日志，仅开发者选项开启时建）、`cpu_coll1`；`boot_scan`/`rescan`（仅重扫场景）。
- 不固定：`touch_init`（开机一次性自删）、`mem_monitor`（性能监视，CONFIG_PERFORMANCE_MONITOR 控制）。
- 任务栈大小已在各创建处按 mem_monitor 高水位实测收紧（LVGL **20KB**（LVGL 9 迁移 6→12KB，TTF 迁移再 12→20KB，见下）、button_svc 4KB、power_svc 3KB、sleep_timer 4KB、deep_worker **12KB**（TTF 迁移由 8KB 上调）、cover_load 6KB、rescan 4KB、mp3_play 8K/out 4K、player_ctrl 4KB；另有主任务 **16KB**（`CONFIG_ESP_MAIN_TASK_STACK_SIZE`，TTF 迁移由 8KB 上调，只在 app_main 期间占用、返回后释放））。⚠ 调栈前先看 HWM 且**必须覆盖目标路径实测**（如自动息屏/亮屏后才打印的读数），勿只信亮屏态读数。
- ⚠ **LVGL 任务栈必须留在内部 RAM，且 TTF 迁移后要 20KB**：v9 的绘制管线（draw task / layer / 各混合函数层层下探）比 8.4 深得多，**6KB 会在"首次渲染设置页"这种重页面时溢出** —— 实测现象是从主界面滑到第 4 页（播放设置）必定 `A stack overflow in task LVGL has been detected` 并 panic 重启；改 12KB 后位图字体时代实测高水位 5532B。⚠ **换成 TTF 字体后光栅化字形会进 FreeType 的平滑光栅化器（它把渲染池放栈上，池子已从默认 16KB 压到 4KB，见 `core/fonts/ftoption_lvgl.h`），12KB 实测只剩 172 字节**，故再上调到 20KB（实测余 ~8.3KB）。加栈会吃内部 RAM，调整前先看 `mem_monitor` 的 stk 行与内部 RAM 余量。凡是**可能同步渲染 LVGL（会光栅化字形）**的任务都要按 ≥12KB 栈预算：目前是主任务 / LVGL / deep_worker 三个。
- **⚠ 内部 RAM 紧张：非实时任务栈一律走 PSRAM**（`core/utils/task_psram.h` 的 `task_create_psram_stack`，失败自动回退内部 RAM）。实测开机末期内部 RAM 仅剩约 4~5KB、DMA 约 2KB、最大连续块约 8KB，内部栈会创建失败——历史症状：`create power service task failed`（自动息屏/低电关机全失效）、重新扫描提示"无法分配栈内存"、performance_monitor 的 mem_monitor/cpu_coll 起不来。已迁到 PSRAM 的任务：`mem_monitor`/`cpu_coll0`/`cpu_coll1`、`batt_log`、`rescan`、`dev_restart`、`cover_load`、`power_svc`、`touch_init`（常驻净省约 12KB）。**保持内部 RAM 的任务**：`LVGL`（每帧刷屏）、`mp3_play`/`mp3_out`（实时解码输出）、`button_svc`（按键中断触发，要求低延迟）、`player_ctrl`/`deep_worker`/`sleep_timer`（交互与深链主路径）、`boot_scan`（扫描要擦写播放列表分区）。新建任务时默认用 `task_create_psram_stack`。
- **⚠ 内部 RAM 最大的单项消耗曾是 debug 的堆毒化（现已默认关闭）**：实测 `CONFIG_HEAP_POISONING_COMPREHENSIVE=y` 多占约 **26KB** 内部 RAM，并把最大连续块从 31.7KB 压到 14.8KB（该模式给每个分配加带调用栈的元数据头且禁止空闲块合并，LVGL 的小对象把它放大成碎片）——这正是"release 从未出现、debug 才出现"内部 RAM 问题的原因，不是版本差异而是配置差异。**2026-09 debug 已改为 `CONFIG_HEAP_POISONING_DISABLED=y`；2026-09-10 又把唯一仍开着它的 releasecandidate 一并改为 `DISABLED`**——实测 RC 开毒化时比 debug 少 18~28KB 内部 RAM、最大连续块被压到 2.8KB（DMA 仅剩 955B/连续 80B），这正是"只有 releasecandidate 才出现"内部 RAM 问题的原因。⚠ **不要重新打开**；需要抓堆破坏/悬垂对象崩溃时，按该文件内注释临时改回 Comprehensive 再构建。实测数据、失败链条与修复验证见 `ideas_and_exps/IRAM_DMA_AUDIT.md`。
- **⚠ LVGL 绘制缓冲是最大的 DMA 内存消费者**：`EXAMPLE_LVGL_BUF_HEIGHT = V_RES/10`（460/10=46 行 → 每块 460×46×2B ≈ 41KB，双缓冲 ≈ 83KB **DMA**；2026-09-10 起已回到此值，前提是 RC 堆毒化关闭——实测 DMA 稳态 **19.4KB / 最大连续块 17.4KB**，SD 读无失败）。实测 DMA 池分布（V_RES/20 那次测量）：启动 263KB → display 后 246KB → audio 后 233KB → SD 后 227KB → **LVGL 后 171KB（绘制缓冲吃掉约 55KB）** → 开机完成 45KB（**这 126KB 的真凶是 `app_ui_create()` 的 LVGL 控件对象约 105KB/+1795 个分配块，不是封面**——封面链路的 .npd/JPEG/像素缓冲全在 PSRAM，见 `IRAM_DMA_AUDIT.md`）。⚠ 原为 V_RES/10（双缓冲 ≈ 83KB DMA），会把 DMA 池压到不足 3KB，导致 SD 卡每次读的 `sdmmc_cmd: allocate_dma_buf: not enough mem`（512B 都分配不到）→ 切歌被误判"坏曲"并自动跳过；该问题在 releasecandidate 上尤其严重（其堆毒化再吃掉 18~28KB）；2026-09-10 关闭毒化后 `V_RES/10` 已稳定运行（DMA 稳态 19.4KB、内部最大连续块 17.4KB、SD 读无失败、睡眠定时器正常）。**调这个值前同时看三项：帧率 + DMA 余量 + 内部 RAM 最大连续块（后者须 ≥4KB：SD 读要 512B、内部任务栈要 4KB）**。（调大→帧率高但 SD 可能失败，调小→SD 安全但滚动掉帧，实测 V_RES/20 约 20fps、V_RES/12 接近原 30fps）。⚠ 缓冲**必须** `MALLOC_CAP_DMA`：非 DMA 缓冲会触发 SPI 驱动的 bounce 缓冲分配（`setup_dma_priv_buffer`），同样从 DMA 池取内存，失败则刷屏阻塞 → 任务看门狗复位。详见 `ideas_and_exps/PSRAM_TASK_STACK.md` 第 4 节。
- 🆕 **LVGL 9 迁移后 DMA/内部 RAM 的格局已改变**：`app_ui_create()` 那批控件对象**不再占内部 RAM**（v9 的 `LV_USE_CUSTOM_MALLOC` + `core/ui/lvgl_mem.c` 把 LVGL 对象堆整体搬到 PSRAM）。因此上面这段"对象吃 105KB 内部 RAM"的历史描述**只适用于 LVGL 8.4 时代**。
- 🆕 **2026-10-16 IRAM 审计（`ideas_and_exps/IRAM_AUDIT_LVGL9.md`）后的当前格局**：先记住一条硬件事实——**ESP32-S3 上 IRAM(0x4037xxxx) 与内部 DRAM(0x3FC8xxxx) 是同一块 SRAM 的两个别名**（`heap/port/esp32s3/memory_layout.c` 的 `soc_memory_regions` 成对给出配对地址），`.iram0.text` 里超出前 16KB 的**每一字节都从内部 DRAM 堆里扣**（map 里的 `.dram0.dummy` 就是它）。所以"代码进 IRAM"与"对象放 PSRAM"是在同一个池子里对冲——这正是"对象都搬去 PSRAM 了、可用内部 RAM 却不见涨"的原因。
  - **IRAM 代码**：166.4KB → **108.9KB**（裁掉 6 种用不到的颜色格式，其中 LVGL 部分 82.8→25.3KB；`LV_ATTRIBUTE_FAST_MEM_USE_IRAM` 保持 y）。
  - **内部 RAM 余量**：仅裁格式时稳态 **84.2KB**；再叠加"路径B"（把两个滚动列表页的热对象钉在内部 RAM）后稳态 **38.8KB**、开机完成最大连续块 **11.3KB**、DMA 最大连续块 **11.3KB**——比 `LVGL9_MIGRATION.md` §10.4 记录的 29.5KB/20KB 更宽裕，且列表快速滚动帧率从 11fps 提到 15fps 量级、慢速滚动 20+fps。
  - ⚠ **改任何与 LVGL 对象分配位置有关的东西，必须同时看帧率和内部 RAM 余量**：本次实测三个性能杠杆的排序是"**对象放哪（内部 RAM 21fps vs PSRAM 11fps）>> blend 放哪（11 vs 9）>> 字体方案（11 vs 12）**"，与"blend 是绘制热点"的直觉相反（滚动开销主要在对象树遍历/布局，对象工作集约 88KB 远超本芯片 32KB 数据 cache）。
  - 详见 `ideas_and_exps/IRAM_AUDIT_LVGL9.md`：§10 四配置对照表、§11 两次失败实验（把对象全部搬回内部 RAM 会直接见底）、§13 路径B 的最终方案与注意点。
- **⚠ 关缓存陷阱：栈在 PSRAM 的任务绝不能触发 Flash 缓存冻结**——`esp_partition_mmap/munmap`、`esp_flash_erase/write`、`pldata_write`/`pldata_erase_range`（内部会 unmap）都会先关外部内存缓存，此时 PSRAM 不可访问，IDF 在 `esp_cache_freeze_caches_disable_interrupts` 断言 `s_task_stack_is_sane_when_cache_frozen()`（要求 sp 在 DRAM），栈在 PSRAM 直接 panic。这类代码要么让任务栈留在内部 RAM，要么用 `deep_worker_call()` 投递到 deep_worker（内部栈）执行——"重新扫描"就是这么做的（见 `ui/settings_menu.c` 的 `rescan_do_deinit`）。判据与实测数据见 `ideas_and_exps/PSRAM_TASK_STACK.md`。
- 息屏（`power_service_screen_off`，经 deep_worker）：AMOLED SLEEP IN（DISPOFF 0x28 + SLEEP IN 0x10）→ `lvgl_port_suspend`（停 tick + 挂起 LVGL/封面任务）→ 固定 80MHz。亮屏（`power_service_screen_on`，经 deep_worker）：固定 240MHz → 持锁 `display_sleep_exit`（SLEEP OUT 0x11 + 120ms + `esp_lcd_panel_init` 重发完整 init + 全屏重刷）→ 恢复亮度 → `lvgl_port_resume`（须先于 LVGL resume，休眠中/刚唤醒面板不能接收刷屏数据）。
- **自动关机策略（power_service 守护任务，1s tick，触发即走既有伪关机链 `fake_shutdown_enter`）**：① 息屏且暂停超时（`CONFIG_PAUSE_AUTO_SHUTDOWN_MIN`，默认 10 分钟）；② 电池电压**持续**低于 **3.45V**（`CONFIG_LOW_BATTERY_SHUTDOWN_SEC`，默认 60 秒，0=关闭；阈值 `BATTERY_CRITICAL_LOW_MV` 与开机拒启共用，见 battery.h）。⚠ 暂无充电检测，充电态电压被抬高不会误触发。
- 🆕 🎯 **"息屏停掉 Core1"（esp_cpu_stall / `CONFIG_SCREEN_OFF_STOP_CORE1`）已从代码与 Kconfig 完全移除**：该方案曾实测失败（长期 esp_cpu_stall 与被停核的 PSRAM/XIP 取指形成总线竞态 → INT WDT 超时），代码不再保留该配置项。勿重新引入；真正的进一步省电方向：暂停/停止时 light sleep 或走既有伪关机。

### 关键技术要点与踩坑记录（务必遵守）

> 硬件/音频/UI 的具体技术细节与踩坑（开机防白屏、显示初始化耗时、触摸异步初始化、触摸数据只读一次、CS43131 采样极性/时钟策略、MP3 采样率限制、音频浮点路径+Clamp、JPEG 封面后台解码、虚拟列表的长列表坐标（v9 起坐标天然 int32_t）、完整字体不运行时加载等）均在各模块源码注释与 `ideas_and_exps/` 设计文档中，读代码/文档时可自然了解；此处仅保留跨项目通用约定。
>
> ⚠ **涉及 LVGL 时先看 `ideas_and_exps/LVGL9_MIGRATION.md`**：项目 2026-10 已从 LVGL 8.4 升到 9.4，部分历史文档（`MARQUEE_STYLE.md`、`VIRTUAL_LIST_USAGE.md`、`IRAM_DMA_AUDIT.md`、`DONE_PLANS.md` 等）里的 API/内存结论是 8.4 时代的，读到时请对照该迁移文档。

- **代码注释一律中文**；函数/变量使用 snake_case 命名，模块前缀（display_/touch_/lvgl_port_/notif_/player_/config_store_ 等）。
- **LVGL 线程安全**：非 LVGL 任务的线程操作 UI 必须先 `lvgl_port_lock(-1)`，用完 `lvgl_port_unlock()`；LVGL 任务回调（timer/event）内已持锁，无需重复加锁。config_store 的 set_xxx 会同步触发已注册回调，UI 注册方须保证回调运行在 LVGL 任务上下文（持锁）。
- **改完即烧录**：首次烧录时，若未指定串口号，则列出串口并询问用户，后续修改完成后直接构建并烧录到对应串口并验证。
- **新模块记得更新 `main/CMakeLists.txt`**：新增源码文件需纳入 `MAIN_SRCS`（部分模块受 Kconfig 条件编译控制，见 CMakeLists 注释）。
- **⚠ 禁止用 shell 文本改写工具编辑源文件（真实事故）**：曾用 `Get-Content -Raw` + `-replace` + `Set-Content` 管道批量删行，**编码往返把 `main/ui/settings_menu.c` 里 163 行中文注释/字符串变成了替换字符（U+FFFD）**——ASCII 代码完好，中文全毁，只能 `git checkout` 取回 HEAD 再逐条重放本次会话的改动（includes、rescan 的 PSRAM 栈 + deep_worker 委托、ClickWheel 覆盖层与开关行），代价是一次完整的恢复流程。规则: 改文件**只用文件编辑工具**（`edit`/`write`），不要用 shell 的 `Set-Content`/`Out-File`/`>` 重定向/`-replace` 管道改写源码；

## 用户背景介绍

### 用户背景

- 用户对嵌入式开发有一定经验（此前完成过 ESP32-P4 时钟项目 OtoClock），代码主要由 AI 协作编写。
- 用户有多年 Java/C# 等高级语言开发经验，擅长设计美观易用的界面，能读懂并修改 C 代码，但不编写 CMake 文件。
- 项目是 DIY 开源项目，注重软硬件自由：固件源码开放，鼓励用户自行修改；不应对固件数据/配置设置不必要的限制。
- 用户会随着项目进度逐渐发掘各 GPIO 的用途（见 GPIO 表），新确认的分配请同步更新文档。

### 用户对代码的要求

- 遵循当前项目已建立的分层与命名习惯（`core/` 底层 vs `core/player`、`core/storage` 等数据层 vs `ui/` 应用、snake_case、模块前缀），新增文件需同时更新 `main/CMakeLists.txt`。
- 代码注释均使用中文，逻辑清晰，关键常量/魔法数字要有注释说明。
- UI 功能按模块实现、逐个测试验证，不要一次性堆砌所有功能。
- 使用 Kconfig（`main/Kconfig.projbuild`）控制可裁剪功能。但通常三套构建配置已经覆盖大部分开启或者关闭的场景。（例如调试版和发布版的差异）
- 所有的过程文档、组件文档会放在根目录下的 ideas_and_exps 文件夹中，新模块建议同步更新 ideas_and_exps/ 对应设计文档。

### 回复/行动说明

- 推理/思考及回复均使用中文。
- 若 IDF 服务器未运行（`idfc` 报 `IDF 服务器可能尚未运行` / 退出码 -1），请提示用户手动启动：执行 `idfc start-server`（普通权限即可；服务器控制台就是启动它的那个窗口，**关掉窗口服务器即停止**）。
