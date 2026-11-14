# MULTI_FORMAT_AUDIO — MP3 / FLAC / OGG 多格式流式播放

> 2026-11 从 hello_world 测试固件反哺。本文记录解码层结构、格式限制、OGG 卡顿修复、息屏频率策略与验证状态。

## 1. 背景

原播放器只支持 MP3（Helix `libhelix-mp3`），曲库扫描也只收录 `.mp3`。本次把播放层改为统一的多格式流式解码：

| 格式 | 解码器 | 打开方式 |
|---|---|---|
| MP3 | minimp3（`minimp3_ex`） | `mp3dec_ex_open_cb()` 文件回调流，`MP3D_DO_NOT_SCAN \| MP3D_SEEK_TO_SAMPLE` |
| FLAC | dr_flac | `drflac_open_file()` 文件流 |
| OGG Vorbis | stb_vorbis | 能放进 PSRAM 连续空闲块时整块读入 + `stb_vorbis_open_memory()`；否则走 512KB stdio 缓冲的文件流（分配失败回退 64KB）。**stb_vorbis 的 setup/temp 内存统一由 1MB PSRAM 分配区提供**，不再使用内部 malloc。 |

第三方单文件库内置在 `main/core/audio_decoders/`，不依赖组件管理器。Helix 托管依赖已从 `main/idf_component.yml` 删除。

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

- 输出统一为交织 16bit PCM，一次最多读 2048 帧。
- MP3 无 Xing/VBR 头时按首帧码率估算总时长，保证进度条和“自然播完”判断正常。
- seek 由各解码器按样本位置执行；MP3 首次 seek 可能需要建索引。

## 4. 播放任务

`core/hardware/audio/mp3_player.c` 保留原 API（`play/pause/resume/stop/seek`、位置/时长查询、停止回调、回放增益），内部改为：

- 单任务 `mp3_play`（Core1，优先级 6），**24KB PSRAM 栈**，不再使用 75KB PCM ringbuf 和独立 `mp3_out` consumer 任务。
- 解码 → 单声道复制 → `audio_pcm_write()` 直接写 I2S；`audio_pcm_write` 内部完成 EQ 与回放增益。
- 暂停/seek 时先 `audio_pcm_stop()`，恢复/seek 完成后 `audio_pcm_start()`。
- 切歌时等待旧任务完全退出（最长 3s），再启动新任务；停止回调在新任务可安全启动前调用（先置 IDLE、清任务句柄）。
- EQ 状态在新曲开始前 `eq_reset_state()`；回放增益在新曲解码器打开后应用。

### 与旧实现的差异 / 风险

- 去掉了 Helix 的 MP3 帧合法性门禁（连续两帧校验）与 CBR 字节比例 seek，改由 minimp3 处理损坏帧与样本 seek。
- 去掉了 75KB ringbuf 的缓冲解耦；抗 SD 抖动依赖 I2S DMA 与解码读取速度。
- `audio_decoders` 与第三方库内部的临时分配默认走内部堆（播放任务自身的 24KB 栈在 PSRAM）；烧录后需用 `mem_monitor` 观察播放期间内部 RAM 波动。
- 因此反哺后必须回归：普通/损坏 MP3、FLAC、OGG、暂停恢复、seek、切歌、坏曲连续跳过、EQ 切换、回放增益、息屏播放。

## 5. OGG 卡顿修复与息屏频率

OGG 在 SD 上卡顿的根因是 stb_vorbis 文件模式内部 `fgetc` 逐字节读取，CPU 占用不高但 SD 小读极多。处理：

- 优先把整文件读入 PSRAM，用 `stb_vorbis_open_memory()` 解码；
- 阈值不再写死 12MB：按当前 PSRAM **最大连续空闲块**动态判断（保留 2MB 给 LVGL/封面/解码器，上限 14MB）。这样 12~14MB 的 OGG 或 PSRAM 稍紧时也能优先走内存路径，而不是固定阈值直接掉到 `fgetc`；
- **stb_vorbis 默认 `malloc` 会把约 100KB 码本/解码缓冲放进内部 RAM**（IDF 的 `SPIRAM_MALLOC_ALWAYSINTERNAL=16KB` 导致大量小块落内部堆），实测打开 OGG 后 DMA 只剩 355B，随后 SD 读 `allocate_dma_buf` 失败——这才是原项目持续卡顿的直接原因。现在通过 `stb_vorbis_alloc` 传入 **1MB PSRAM 分配区**，setup/temp 内存全部移出内部 RAM；分配失败才退回库内部 malloc 并打警告。本机样本实测 setup 需求约 **190~203KB**，1MB 足够；宿主端用 `stb_vorbis_open_memory()` 正常解码验证通过。
- 超过可用内存时走文件流，stdio 缓冲从 64KB 提升到 **512KB**（可覆盖多个 OGG 页），分配失败再回退 64KB；
- 文件流日志会打印 `file=... largest PSRAM=... alloc=PSRAM/internal`，方便判断当前走的是 `memory` 还是 `file+buffered`，以及 stb_vorbis 内存落在哪。
- 如果仍卡顿，先看串口这行：
  `OGG stream opened: ... (memory/file+buffered, file=... B, largest PSRAM=... B, alloc=PSRAM/internal)`；
  `memory` 仍卡说明不是 SD 读取问题，应继续查 CPU/PSRAM 总线/音频输出；`file+buffered` 则说明文件太大或 PSRAM 连续块不足。
- 息屏播放时 CPU 固定 **320MHz**（`power_service` 每秒按 `mp3_player_get_state()` 切换）；暂停/停止后回落到 **160MHz** 省电。

亮屏固定 320MHz。原息屏 80MHz 对 OGG/FLAC 会欠载，已废弃。

## 6. 构建

`main/CMakeLists.txt`：

- 加入 `core/audio_decoders/audio_decoder.c`、`minimp3_impl.c`、`dr_flac_impl.c`；
- 对这三个源文件设置 `-w`，避免第三方库的未使用变量等警告被 `-Werror` 拦下；
- `core/music/cover_art.c` 与播放器改动在同一提交。

`main/idf_component.yml` 删除 `chmorgan/esp-libhelix-mp3`；`dependencies.lock` 同步移除。根 `CMakeLists.txt` 不再需要 `esp_ringbuf`（新播放器无 ringbuf；IDF 仍可能因其它依赖把它拉进构建，属正常）。

## 7. 验证状态

- 本仓库：ESP-IDF 6.1 编译通过（app 0x295220，分区余量 57%）。
- stb_vorbis 分配器宿主测试：56KB / 557KB OGG 样本均能打开并解码，`setup_memory_required` 约 190~203KB，1MB PSRAM 分配区足够。
- hello_world 测试固件：MP3/FLAC/OGG 真机播放、>32MB 文件、OGG 息屏卡顿修复已验证。
- 反哺到本仓库后尚未重新烧录真机；烧录后按 §4 的回归清单逐项确认。

## 8. 相关文档

- `PLAYLIST_SCAN_SUMMARY.md`：曲库扫描与指纹。
- `COVER_ART.md`：内嵌/目录封面。
- `PLAYER_CONTROLLER_SUMMARY.md`：播放控制与停止回调。
