# mbedtls 能否关掉并从构建中排除（2026-11 调研）

> 触发问题：「项目不用任何网络/HTTPS，mbedtls 虽然不会嵌入，但会占用全量编译时间，能不能关掉并从构建中排除？」
>
> **调研方式：纯只读**（IDF 源码 + 上次构建留下的产物）。没有跑计时构建、没有改任何文件，
> 所以本文的时间类数字是**编译单元占比**，不是实测墙钟。证据全部可用本文 §7 的脚本复现。

## 1. 结论摘要

| 问题 | 答案 |
|---|---|
| 有 Kconfig 能关掉 mbedtls 吗？ | **没有**。mbedtls 不是可裁组件，只由组件依赖进构建 |
| 能"从构建中排除"吗？ | **不能干净排除**：被 IDF 两处**无条件**依赖钉住（§3） |
| 它进固件吗？ | **几乎不进**——最终镜像里只有 **2 518 B / 8 个目标文件**（占 0.13%） |
| 它花多少编译时间？ | 153 个编译单元 / 4.66 MB 源码 = 全部 **9.2 % / 11.8 %**（§4） |
| 值得为它打 IDF 补丁吗？ | **不值得**（§5-A：要改 2 处 CMakeLists + 重写 `bootloader_sha` 的哈希） |
| 那"全量编译慢"该动哪？ | **组件剪裁**（`COMPONENTS`）：155 → 71 组件、1 665 → ≈1 195 编译单元（§5-B） |

## 2. 事实一：它确实基本不进固件（量化）

`build/debug/FakePodNanoNightPlayer.map` 中按"非 `.debug*` 段 + 地址非 0（不是 discarded）"统计：

| 项 | 值 |
|---|---|
| 最终链接进镜像的 mbedtls 目标文件 | **8 个**（153 个编译单元里） |
| 占用字节 | **2 518 B ≈ 2.46 KB** |
| 占 app（1 862.8 KB） | **0.13 %** |

留下的就是 PSA 初始化胶水，**没有任何 SSL / X509 / 证书代码**：

```
766  tf-psa-crypto/core/libtfpsacrypto.a(psa_crypto.c.obj)
642  tf-psa-crypto/core/libtfpsacrypto.a(psa_crypto_slot_management.c.obj)
226  tf-psa-crypto/core/libtfpsacrypto.a(threading.c.obj)
212  tf-psa-crypto/core/libtfpsacrypto.a(psa_crypto_driver_esp_rsa_ds.c.obj)
198  tf-psa-crypto/core/libtfpsacrypto.a(platform_util.c.obj)
164  mbedtls/library/libmbedtls.a(esp_psa_crypto_init.c.obj)
156  libmbedtls.a(esp_mem.c.obj)
154  tf-psa-crypto/core/libtfpsacrypto.a(platform.c.obj)
```

其余 145 个目标文件虽然被**编译**，但链接期被 `--gc-sections` 全部丢弃
（map 的 "Discarded input sections" 里它们全是地址 `0x0`）。
⚠ 所以「省 Flash」不是做这件事的理由，**唯一动机只有编译时间**。

## 3. 事实二：为什么它一定会被编译（两条硬依赖）

### 3.1 `bootloader_support`（app 构建分支硬写）

`C:\esp\v6.1\esp-idf\components\bootloader_support\CMakeLists.txt:102`：

```cmake
    # heap is required for `heap_memory_layout.h` header
    set(priv_requires spi_flash mbedtls efuse heap esp_bootloader_format esp_app_format esptool_py)
```

这是 **else 分支（非 BOOTLOADER_BUILD）里的无条件列表**，没有任何 `CONFIG_*` 包裹。
而且它不是"写了但没用"——`components/bootloader_support/src/bootloader_sha.c` 真的：

```c
#include "psa/crypto.h"
```

并调用 PSA 哈希算镜像 SHA-256。map 里的链接原因写得很清楚：

```
esp-idf/bootloader_support/libbootloader_support.a(bootloader_sha.c.obj) (psa_hash_abort)
  → esp-idf/mbedtls/.../libtfpsacrypto.a(psa_crypto.c.obj)
```

（`bootloader_support` 本身必进构建：`main → … → esp_hw_support → bootloader_support`；
BFS 找到的最短链是 `main → lvgl → log → esp_hw_support → bootloader_support → mbedtls`，
注意这条链混了 public/private 边，只作示意。）

### 3.2 `nvs_flash`（与 `CONFIG_NVS_ENCRYPTION` 无关）

`C:\esp\v6.1\esp-idf\components\nvs_flash\CMakeLists.txt:86-89`（非 Linux、非 TEE 的常规 OS 构建）：

```cmake
    else()
        target_sources(${COMPONENT_LIB} PRIVATE "src/nvs_encrypted_partition.cpp"
                                                "src/nvs_bootloader_aes.c"
                                                "src/nvs_bootloader_xts_aes.c")
        target_link_libraries(${COMPONENT_LIB} PRIVATE idf::mbedtls)
    endif()
```

**即使 `CONFIG_NVS_ENCRYPTION=n`，NVS 加密相关源文件也照编、也照链 mbedtls**。
map 里的证据：`libnvs_flash.a(nvs_encrypted_partition.cpp.obj) (esp_aes_xts_init)`。

### 3.3 全量构建（数量级真正的原因）

`build/debug/project_description.json`：`build_components` = **155 个**，编译单元 **1 665 个**。
不是"依赖它们"，而是**工程没有设置 `COMPONENTS`**：

- `C:\esp\v6.1\esp-idf\tools\cmake\build.cmake:588-597`：
  `COMPONENTS` … used to trim the build … **"If not specified, all components known to the
  build system are processed."**
- 官方文档 `C:\esp\v6.1\esp-idf\docs\en\api-guides\build-system.rst:253`、`:625-632`：
  默认 = `COMPONENT_DIRS` 里全部组件；用 `COMPONENTS`（或 `MINIMAL_BUILD` 属性）可显著缩短编译时间。

因此在当前配置下，`esp_wifi` / `bt` / `lwip` / `wpa_supplicant` / `esp_http_client` /
`esp_https_ota` / `protocomm` / `openthread` / `unity` / `cmock` / `esp_eth` … 全都被编了一遍，
**mbedtls 只是这 155 个里的一个**（虽然它是第二大）。
（bootloader 子工程只有 39 个组件、**不含 mbedtls**，所以这笔开销全在 app 侧。）

### 3.4 顺带搞清的机制：`main` 为什么能 include 所有组件的头文件

`C:\esp\v6.1\esp-idf\tools\cmake\project.cmake:892-917` 有对 `main` 的**特殊待遇**：
若用户没有手写 `REQUIRES` / `PRIV_REQUIRES`（只有组件管理器加的那些），
就让 `main` 直接 `INTERFACE_LINK_LIBRARIES = 构建里的其它所有组件`。

本工程正是这种情况——`project_description.json` 里 `main` 的 reqs 只有
`lvgl__lvgl` + `espressif__esp_lcd_sh8601`（priv 是 libhelix / esp_jpeg / esp_lcd_touch / freetype，
全部来自 `main/idf_component.yml`），而 `main` 里照样能 include `nvs_flash.h` / `esp_vfs_fat.h` /
`esp_adc/adc_oneshot.h` / `sdmmc_cmd.h` 等。

⚠ **这条机制是剪裁组件时最大的坑**（见 §5-B 的风险）。

## 4. 编译成本量化

`build/debug/compile_commands.json`（debug 构建，共 **1 665** 个编译单元 / **39.6 MB** 源码）：

| 组件 | 编译文件数 | 源码量 |
|---|---|---|
| lvgl（托管） | 527 | 17.1 MB |
| **mbedtls** | **153** | **4.66 MB** |
| lwip | 91 | 2.5 MB |
| wpa_supplicant | 84 | 1.6 MB |
| PROJECT(main) | 54 | 1.75 MB |
| fatfs | 12 | 2.3 MB |

⇒ **mbedtls ≈ 9.2 % 的编译单元、11.8 % 的源码字节**。

153 个文件的构成（说明"关某个 Kconfig 能不能少编"）：

| 区域 | 文件数 |
|---|---|
| tf-psa-crypto/drivers | 52 |
| mbedtls/library（x509 / ssl / pkcs7 …） | 32 |
| port/psa_driver | 13 |
| tf-psa-crypto/extras + core + utilities + platform | 31 |
| port/aes + sha + ecc + bignum + 其它 | 24 |
| esp_crt_bundle/esp_crt_bundle.c | 1 |

**ccache 已经开着**（`build/debug/CMakeCache.txt`：`CCACHE_ENABLE=True`，
`ccache 4.12.1`）。所以这约 10 % 只落在**冷编译**（首次 / `fullclean` / 换 IDF 或编译器 /
改 mbedtls 配置）上，日常增量构建为 0。

## 5. 三条可选路线

### A. 真把 mbedtls 从构建里摘掉（**不推荐**）

需要的改动（全部是 **IDF 源码改动**，与 `fatfs/src/ffconf.h` 的 `FF_FS_EXFAT` 同类：
换机器 / 重装 / 升级 IDF 必丢，需在迁移文档里再记一笔）：

1. `bootloader_support/CMakeLists.txt:102` 去掉 `mbedtls`；
2. 但紧接着 `bootloader_sha.c` 编不过（`psa/crypto.h` 不在），必须把
   `bootloader_sha256_*` 换回 ROM SHA / `esp_sha` 实现——这是**行为改动**；
3. `nvs_flash/CMakeLists.txt:86-89` 去掉那 3 个源文件与 `idf::mbedtls` 链接
   （等于放弃 NVS 加密能力，当前也没用）。

代价：**只换回约 10 % 冷编译时间**，却要重测 `app_update` 镜像校验、`esp_image_format`、
分区表/OTA 路径与 NVS；且以后每次 IDF 升级都要重打。**性价比明显为负。**

### B. 组件剪裁（`COMPONENTS` / `MINIMAL_BUILD`）—— 真正的大头

按 `main` 的依赖闭包（BFS 过 `project_description.json` 的 reqs/priv_reqs）估算：

| | 现在 | 剪裁后 |
|---|---|---|
| 组件数 | 155 | **71** |
| 编译单元 | 1 665 | **≈1 195（≈ −28 %）** |
| wifi / bt / lwip / http / ota / protocomm / unity / cmock | 全编 | 全不编 |
| mbedtls | 153 | **仍是 153**（走 `bootloader_support`） |

写法（必须在 `include(project.cmake)` **之前**，与现有 `set(IDF_TARGET …)` 同一位置）：

```cmake
# 精简组件表（草案，**未构建验证**）
set(COMPONENTS main
    esp_lcd_qspi_amoled esp_lcd_touch_cst816 esp_lcd_touch_cst820 esp_lcd_touch_chsc6417
    nvs_flash fatfs sdmmc vfs spiffs esp_adc esp_lcd driver esp_partition
    esp_timer esp_psram esp_pm esp_ringbuf spi_flash)
```

⚠ **三个必须知道的坑**：

1. **不能只写 `MINIMAL_BUILD ON` 或 `COMPONENTS main`**。§3.4 那个"main 链接全部组件"的特殊待遇
   只在**构建里的组件**范围内生效；一旦剪裁，`main` 立刻丢失被剪掉组件的头文件。
   本工程至少会缺：`nvs_flash`、`fatfs`、`sdmmc`、`vfs`、`spiffs`、`esp_adc`、`driver`、`esp_partition`…
   （`esp_lcd` / `esp_timer` / `esp_psram` / `esp_pm` 因为被 lvgl / esp_lcd_sh8601 依赖，可能在闭包里）
2. **本仓库自带的 4 个本地组件必须显式列出**：`esp_lcd_qspi_amoled`（[display.h:5](../main/core/hardware/display.h#L5)）
   与 `esp_lcd_touch_cst816/820/chsc6417`（[touch.c:33](../main/core/hardware/touch.c#L33) / `:38` / `:43`）。
   它们**没有**被 `main` 依赖，今天是靠"全量构建 + main 链接全部组件"才编上、链上的。
3. 反过来，**将来新增任何组件依赖，如果忘了写进 `COMPONENTS` 就会编译报错**（错误明显，不算隐蔽），
   代价是这份白名单要维护；`main` 一旦手写 `REQUIRES`，§3.4 的"链接全部组件"便利就永久失效。

实施建议：**不要一次写全**。从 `main` + 4 个本地组件起步，构建一次，按编译器报的缺失头文件逐个补，
2~3 轮即可收敛；收敛后必须做一次真机回归（SD/FATFS、NVS 配置、字体、音频、触摸一个都不能少）。

### C. 零风险小优化（不碰依赖图）

当前 [sdkconfig](../sdkconfig) 里有两条纯粹浪费（项目不校验证书、不解析 PKCS#7）：

| 配置 | 现状 | 关掉后 |
|---|---|---|
| `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE` | `y` + `_DEFAULT_FULL` | 少编 `esp_crt_bundle.c`，并**跳过 `gen_crt_bundle.py` 生成证书包这一步** |
| `CONFIG_MBEDTLS_PKCS7_C` | `y` | 少编 `pkcs7.c` |

量级说明：**只砍两三个文件**，不改变数量级，属于顺手做；真正的杠杆是 §5-B。

## 6. 建议

1. **不要为 mbedtls 去打 IDF 补丁**（§5-A 性价比为负）。
2. 若要缩短冷编译：做 **§5-B 的 `COMPONENTS` 剪裁**（−28 % 编译单元，且以后不需要再手动关东西），
   按"起步最小集 + 编译报错迭代"的方式收敛，之后做一次全功能真机回归。
3. 顺手做 §5-C 两条（几乎零风险）。
4. 冷编译想再快，先看 `ccache -s` 命中率，而不是继续抠单个组件。

## 7. 复现方法（只读）

```powershell
# 组件清单与依赖图
python -c "import json;d=json.load(open('build/debug/project_description.json'));print(len(d['build_components']),len(d['common_component_reqs']))"

# 各组件编译单元与源码字节（mbedtls 占比）
#   见本文的统计口径：compile_commands.json 里按 esp-idf/components/<名>/ 归组

# mbedtls 真正进镜像的字节数
#   在 .map 里筛 "esp-idf/mbedtls/" 且地址非 0（地址 0 = discarded）的行，累加第三列
```

## 8. 未验证 / 待办

- [ ] `COMPONENTS` 草案清单**未经构建验证**（本次只读，且当时另一 agent 在跑构建）。
- [ ] 剪裁后的编译单元数（估算 ≈1 195）与冷编译墙钟收益**需实测**。
- [ ] 剪裁后必须回归：SD/FATFS 挂载、NVS 读写（配置槽位）、字体分区读取、
      音频（I2S/CS43131）、触摸三型号识别、OTA/分区表相关路径。
- [ ] 若哪天真的要摘 mbedtls，先确认 `bootloader_sha.c` 改 ROM SHA 后
      `app_update` / `esp_image_format` 的校验链有对应用例（目前项目不做 OTA，风险集中在
      "以后加 OTA 时要重做一遍"）。
