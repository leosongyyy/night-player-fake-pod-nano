# MULTI_FORMAT_AUDIO — MP3 / FLAC / OGG 多格式流式播放

> 2026-11 从 hello_world 测试固件反哺。本文记录解码层结构、格式限制、OGG/Tremor、息屏 80MHz 与播放爆音修复的验证状态。

## 1. 背景

原播放器只支持 MP3（Helix `libhelix-mp3`），曲库扫描也只收录 `.mp3`。本次把播放层改为统一的多格式流式解码：

| 格式 | 解码器 | 打开方式 |
|---|---|---|
| MP3 | minimp3（`minimp3_ex`） | `mp3dec_ex_open_cb()` + 16KB stdio 读缓冲，`MP3D_DO_NOT_SCAN \| MP3D_SEEK_TO_SAMPLE` |
| FLAC | dr_flac | 自定义 read/seek/tell 回调 + 16KB stdio 读缓冲（可 setvbuf，close 时自行 fclose） |
| OGG Vorbis | **Tremor / libvorbisidec** | `ov_open()` 文件流 + 64KB stdio 读缓冲；仅 Vorbis |

Tremor 是 **定点整数** Vorbis 解码器，配 libogg 容器库；源码在 `components/tremor` 与 `components/libogg`。OGG 的所有堆分配通过 `ogg_psram_alloc_init()` 的 **1MB PSRAM 专用 arena**（`multi_heap`），不再使用默认 `malloc`。

Helix 托管依赖已从 `main/idf_component.yml` 删除；旧的 `main/core/audio_decoders/stb_vorbis.c` 已移除。

## 2. 支持范围与限制

- 扩展名：`.mp3` / `.flac` / `.ogg`（大小写不敏感）。
- 采样率：**32kHz / 44.1kHz / 48kHz**；其它采样率按“格式不支持”跳过并提示。
- 声道：单声道、双声道；单声道在播放任务内复制成双声道。
- OGG：仅 Vorbis，不支持 Opus。
- 不支持：APE、WAV、ALAC、AAC、Opus。
- 曲库扫描的 `is_audio_file()` 与播放器使用同一套扩展名；pldata 版本升到 **v3**，升级后旧缓存自动重扫一次。

## 3. 解码层接口

`core/audio_decoders/audio_decoder.h` 提供：

```c
audio_decoder_t *audio_decoder_open(const char *path, char *err, size_t err_len);
int  audio_decoder_read(audio_decoder_t *dec, int16_t *out, int max_frames);
uint32_t audio_decoder_sample_rate(const audio_decoder_t *dec);
uint32_t audio_decoder_channels(const audio_decoder_t *dec);
uint64_t audio_decoder_total_frames(const audio_decoder_t *dec);
esp_err_t audio_decoder_seek(audio_decoder_t *dec, uint32_t seconds);
void audio_decoder_close(audio_decoder_t *dec);
```

- 输出统一为交织 16bit PCM；播放任务每次请求 **512 帧**。
- MP3 无 Xing/VBR 头时按首帧码率估算总时长。
- OGG 用 `ov_read()`；遇到 `OV_HOLE`（损坏页）最多跳过 8 次，避免坏块直接中断整轨。
- OGG seek 使用 `ov_pcm_seek()`；FLAC 使用 `drflac_seek_to_pcm_frame()`；MP3 用样本位置 seek。
- seek 由各解码器按样本位置执行；MP3 首次 seek 可能需要建索引。

## 4. 播放任务与爆音修复

`core/hardware/audio/mp3_player.c` 保留原 API（`play/pause/resume/stop/seek`、位置/时长查询、停止回调、回放增益），关键参数：

- 单任务 `mp3_play`（Core1，**优先级 9**，**64KB PSRAM 栈**）；Tremor 首次解码的 codebook 索引需要较大栈。
- 每次解码 **512 帧**（约 11.6ms @44.1k），PCM 缓冲 2KB 优先放内部 RAM，失败再退 PSRAM。
- 不再使用 75KB PCM ringbuf 和独立 `mp3_out` consumer 任务。
- 解码 → 单声道复制 → `audio_pcm_write()` 直接写 I2S；EQ 与回放增益在 `audio_pcm_write` 内处理。
- 暂停/seek 时先 `audio_pcm_stop()`，恢复/seek 完成后 `audio_pcm_start()`。
- 切歌时等待旧任务完全退出（最长 3s），再启动新任务；停止回调在新任务可安全启动前调用。

`core/hardware/audio/audio.c` 的爆音修复：

- **I2S DMA 加深到 8 × 511 帧（约 93ms @44.1k）**，并在 `i2s_new_channel()` 前显式写入 `chan_cfg.dma_desc_num/frame_num`；511 是为避开单描述符 4092 字节上限（512×8=4096 会被截断）。
- 切采样率时用覆盖整条 DMA 链的静音 preload；preload 不足时打警告。
- `audio_pcm_write` 在 **EQ 关闭且回放增益为 1.0** 时走整数快路径：`int16 × 65536` 直接输出 int32，与浮点路径逐位等价，减少逐样本浮点/EQ 调用；其它情况仍走原浮点 + Clamp 路径。
- 这些改动与 80MHz 息屏一起消除了低频下 I2S DMA 欠载产生的爆音/咔哒。

## 5. OGG/Tremor 与内存

之前 stb_vorbis 的默认 `malloc` 会把约 100KB 码本/解码缓冲放进内部 RAM；实测打开 OGG 后 DMA 只剩 355B，SD 读 `allocate_dma_buf` 随即失败，表现为持续卡顿/坏曲。

现在：

- `ogg_psram_alloc_init()` 建立 **1MB PSRAM arena**（失败时逐块 PSRAM 回退），libogg/Tremor 的 `_ogg_malloc/calloc/realloc/free` 全部走它；
- OGG 文件通过 64KB stdio 缓冲流式读取，不再整文件读入 PSRAM；
- 打开日志：`OGG/Tremor stream opened: ...`；arena 初始化日志：`OGG arena ready: 1MB PSRAM ...`；
- 这样 OGG 解码不再挤占 SDMMC/I2S 依赖的内部 DMA 池。

## 6. 息屏频率

- 亮屏固定 **320MHz**；
- 息屏统一固定 **80MHz**（播放/暂停/停止不再区分频点）。Tremor 是定点解码，80MHz 下仍能实时解码；I2S DMA 已加深到约 93ms，吸收低频抖动；
- 不再有 160MHz 空闲档和 320MHz 播放档；`power_service` 每秒只做低电保护与自动息屏检查。

## 7. 构建

- 根 `CMakeLists.txt` 的 `COMPONENTS` 白名单加入 `tremor`；`tremor` 通过 `REQUIRES libogg` 自动带入 `libogg`。
- `components/libogg`：bitwise/framing/ogg_psram_alloc，固定 `-O2`。
- `components/tremor`：定点 Vorbis 解码，固定 `-O2 -fno-strict-aliasing`。
- `main/CMakeLists.txt` 仍编译 minimp3/dr_flac 单文件库并对其关警告；`stb_vorbis.c` 已删除。
- 不再需要整文件 OGG PSRAM 分配，因此没有 `OGG_MEMORY_MAX`/512KB 文件流分支。

## 8. 验证状态

- 本仓库：ESP-IDF 6.1 编译通过（app 0x2A2BC0，分区余量 56%）。
- hello_world 测试固件：Tremor OGG 播放、80MHz 息屏、I2S DMA 加深与爆音修复已在真机验证。
- 反哺到本仓库后尚未重新烧录真机；烧录后重点回归：
  - MP3/FLAC/OGG 播放与切歌；
  - 损坏 OGG 页（`OV_HOLE`）跳过；
  - 暂停/恢复/seek；
  - 息屏播放无爆音、无卡顿；
  - EQ 开启/关闭与回放增益非 1.0 时仍正常；
  - 坏曲连续跳过、SD 读无 `allocate_dma_buf` 失败。

## 9. 相关文档

- `PLAYLIST_SCAN_SUMMARY.md`：曲库扫描与指纹。
- `COVER_ART.md`：内嵌/目录封面。
- `PLAYER_CONTROLLER_SUMMARY.md`：播放控制与停止回调。
