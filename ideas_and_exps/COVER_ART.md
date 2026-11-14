# COVER_ART — 内嵌/目录封面与封面尺寸适配

> 2026-11 从 hello_world 测试固件反哺。本文记录封面查找顺序、支持的标签格式、大小限制与大图缩放策略。

## 1. 封面查找顺序

`ui/now_playing.c` 的封面任务按以下顺序取图：

1. **`.npd` 元数据**里的 JPEG（电脑端扫描工具生成，仍是首选）；
2. **音频内嵌封面**（`core/music/cover_art.c`）：
   - MP3：ID3v2.2 `PIC`，ID3v2.3/2.4 `APIC`；
   - FLAC：`PICTURE` 元数据块；
   - OGG Vorbis：Vorbis comment 的 `METADATA_BLOCK_PICTURE`（base64）或 `COVERART`（base64）。
3. **同目录封面图片**：`cover` / `folder` / `front` / `album` / `albumart` / `artwork` / `封面` 等命名的 `.jpg` / `.jpeg`；没有命中关键词时，只有目录里恰好一张 JPEG 才会使用。
4. 都没有 → 内置默认封面。

内嵌图片优先 front cover（picture type = 3）；没有 front 时使用第一张可解析的 JPEG。

## 2. 格式与大小限制

- **只支持 JPEG**（`cover_jpeg_decode()` 只解 JPEG）；PNG 会被跳过并打印日志，建议转成 `cover.jpg`。
- 单张 JPEG 上限 **2MB**。
- ID3 标签 / FLAC 图片块上限 **4MB**（含 padding 与其它帧）。
- OGG 注释包上限 **4MB**。
- 返回的 JPEG 缓冲优先分配在 PSRAM，由 `cover_art_free()` 释放。

## 3. 封面尺寸适配

### 3.1 软件解码

`core/image/cover_jpeg.c` 的软件路径（esp_jpeg/tjpgd）支持到 **4096×4096**：

| 原图最长边 | 解码缩放 | 最终尺寸 |
|---|---|---|
| ≤1024 | 原尺寸 | ≤1024 |
| 1024~2048 | `JPEG_IMAGE_SCALE_1_2` | ≤1024 |
| 2048~4096 | `JPEG_IMAGE_SCALE_1_4` | ≤1024 |

像素缓冲按缩放后尺寸向上取整，并多留 16px 吸收 MCU 对齐差异，避免 PSRAM 峰值过大。

### 3.2 硬件解码

ESP32-S31 硬件 JPEG 路径仍限制 **512×512**（受当前驱动/内存策略限制）。大图会先尝试硬件，发现尺寸超限后自动回退软件解码，封面不会丢。460×460 的 `.npd` 封面仍走硬件路径（约 20ms）。

### 3.3 显示

正在播放页的封面控件固定为 460×460，并设置 `lv_image_set_inner_align(..., LV_IMAGE_ALIGN_CENTER)`：

- 大于屏幕：居中裁切；
- 小于屏幕：居中留黑边；
- 不再要求封面必须是 460×460。

封面任务栈从 6KB 提升到 **8KB**，覆盖 ID3/FLAC/OGG 标签解析与目录扫描的局部缓冲。

## 4. 内存与线程

- 封面查找/解码在 `cover_load`（Core1、低优先级）任务执行，不抢音频解码。
- PSRAM 优先，失败回退默认堆；像素缓冲使用 64B 对齐分配（LVGL PPA/绘制要求）。
- 切歌后快速校验“当前歌曲未变”再应用封面，避免错图。

## 5. 验证状态

- 本仓库：ESP-IDF 6.1 编译通过。
- hello_world 测试固件：内嵌封面、目录封面、大图缩放与居中裁切已验证；测试曲目无内嵌封面时正确回退默认图。
- 反哺后尚未重新烧录真机；建议用以下样本回归：
  - 带 APIC 的 MP3；
  - 带 PICTURE 的 FLAC；
  - 带 `METADATA_BLOCK_PICTURE` 的 OGG；
  - 1000×1000 / 3000×3000 的 `cover.jpg`；
  - 只有 PNG 封面的目录（应回退默认图并打日志）。

## 6. 相关文档

- `COVER_JPEG_HW.md`：硬件 JPEG 解码接线、MCU 对齐与 PPA stride 坑。
- `MULTI_FORMAT_AUDIO.md`：多格式播放。
