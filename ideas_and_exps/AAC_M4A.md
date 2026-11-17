# AAC_M4A — m4a / aac（AAC-LC）播放支持

> 2026-11：FakePodNano Pro（S31）在既有 MP3/FLAC/OGG/WAV 之上新增 `.m4a`（MP4 容器 + AAC-LC）与 `.aac`（ADTS 裸流）播放。本文记录实现结构、许可证、关键设计（残帧/seek/坏曲/内存）与真机验证清单。

## 1. 范围

- **做**：有损 AAC-LC（Audio Object Type = 2），32/44.1/48kHz，单/双声道；m4a 内嵌 JPEG 封面（covr）。
- **不做**：ALAC、Opus、HE-AAC（SBR/PS）、MAIN/SSR/LTP、22.05/96kHz。
- 遇到不支持/损坏内容：打开或读取时明确报错跳过，**绝不输出满幅 PCM**（对照 `MULTI_FORMAT_AUDIO.md §4.1` 的 WAV 尾部噪声事故）。

## 2. 分层与文件

```
audio_decoder.c        薄分发：扩展名 → AUDIO_DEC_AAC，统一字段搬运
  └─ aac_decoder.c     会话：打开/读取/seek/关闭、1024→512 残帧、PSRAM 归属
       ├─ mp4_demux.c  ISO BMFF box 遍历、esds→ASC、stbl sample 表、covr
       ├─ adts_parser.c ADTS 同步/帧计数、比例 seek 重同步
       └─ components/libhelix-aac/  vendored 定点 AAC-LC 解码器
```

新增源文件：`aac_decoder.h/.c`、`mp4_demux.h/.c`、`adts_parser.h/.c`；
新增组件：`components/libhelix-aac/`；新增 Kconfig：`CONFIG_AUDIO_ENABLE_AAC`（默认 y）。

## 3. 许可证（必须知晓）

- `components/libhelix-aac/` 的 C 源码来自 `https://gitee.com/open-vela/external_libhelix-aac`（dev），
  每个文件的 **RealNetworks RPSL/RCSL 版权头原样保留**。
- 许可全文 `LICENSE.RPSL` 取自 `https://github.com/Microchip-MPLAB-Harmony/helix_mp3/blob/master/fixpnt/RPSL.txt`
  （RealNetworks Public Source License Version 1.0, 2002-10-28）。
- **本组件为 RPSL-1.0 / RCSL-1.0 双许可，不是 Apache-2.0**；项目主体 MIT 不覆盖该目录，
  作为许可证例外处理。商业分发与 AAC 专利授权需分发方自行评估。
- AOSP 官方项目列表中不存在 `external/libhelix-aac`（只有 `external/aac`=Fraunhofer FDK 与
  `external/libxaac`），架构文档早期"AOSP Helix = Apache-2.0"的假设不成立，已按真实许可入库。

## 4. 关键设计

### 4.1 残帧缓冲（最容易出错）

Helix `AACDecode()` 每次输出 **1024 帧 × channels**，播放任务每次只要 **512 帧**。
`aac_decoder.c` 常驻 `pcm_full`（1024×2×2 = 4KB，PSRAM）+ `full_pos` 游标：

```
read(512): 先吐 full_pos..full_frames（第一次 512），不足再解下一帧补满。
EOF/错误：先吐完已有残帧，再返回 0 / -1。
AACDecode 返回非 0：不写 pcm_full、不推进游标，本次已吐出的合法帧照常返回。
```

seek 后必须 `AACFlushCodec()` 并清 `full_frames/full_pos`，否则旧位置的残帧会错位播放。

### 4.2 采样率与声道

- ASC：`audioObjectType==2` 才放行；`samplingFrequencyIndex`（或显式 24bit 频率）只接受 32/44.1/48k；
  `channelConfiguration` 只接受 1/2。
- ADTS：同步字 0xFFF + layer==0 + profile==1（LC）+ 每帧 1 个 raw block + 采样率索引 3/4/5 + 声道 1/2。
- 播放任务 `mp3_player.c:94-103` 的白名单与单声道复制链保持不变，对新格式天然生效。

### 4.3 seek

- m4a：`stts` 逐项累加 `(sample_count, sample_delta)` 把秒换成 sample index，再按
  `stsc/stco/co64/stsz` 精确定位 chunk 内 sample；`total_frames = sample_count × 1024`。
- ADTS：打开时全文件只读 7 字节头逐帧数出 `frames_total`（精确时长）；seek 按
  `target_frame / frames_total` 估算字节偏移，再从落点向后重同步，**最多 64 帧 / 64KB**，
  误差 ≤ 1 帧（≈23ms）。重同步失败保持原游标不变。

### 4.4 内存归属

| 对象 | 大小 | 归属 |
|---|---|---|
| Helix 状态 arena | 32KB（实测占用约 20.7KB） | PSRAM，`AACInitDecoderPre()` carving |
| 残帧 `pcm_full` | 4KB | PSRAM |
| 单帧原始 AAC | 8KB | PSRAM |
| stdio IO 缓冲 | 16KB | PSRAM（`setvbuf`） |
| MP4 stsz/stco/co64/stts/stsc/ASC | 按 sample 数精确分配 | PSRAM |
| ADTS 同步扫描前缀 | ≤ 64KB（临时） | PSRAM，用完即释放 |

⚠ Helix 用 `AACInitDecoderPre()` 而不是 `AACInitDecoder()`；关闭时**不调用 `AACFreeDecoder()`**，
否则 `FreeBuffers()` 会 `free()` 掉 arena 内部指针（非法释放）。分配失败一律返回明确错误，不降级到内部 RAM。

### 4.5 封面与缓存

- `cover_art.c` 的 `ca_mp4_find()` 解析 `moov/udta/meta/ilst/covr/data`，只认 JPEG 魔数；
  PNG covr 打一行日志后回退。回退链 `.npd → 内嵌 → 同目录 → 默认图` 不变。
- `playlist_builder.c` 的 `is_audio_file()` 与分发层扩展名集合一致；`playlist_store.c`
  的 `PLAYLIST_STORE_VERSION` 由 v4 升到 **v5**（PLST 布局不变），旧缓存自动重扫一次。

## 5. 构建与裁剪

- 根 `CMakeLists.txt` 的 `COMPONENTS` 白名单加 `libhelix-aac`；
  `main/CMakeLists.txt` 的 `MAIN_SRCS_PLAYER` 在 `CONFIG_AUDIO_ENABLE_AAC=y` 时追加三个 C 文件。
- 组件固定 `-O2 -fno-strict-aliasing -w -DCONFIG_LIB_HELIX_AAC_PNS=1`（PNS 是 AAC-LC 工具，
  `pns.c` 的实现由该宏保护）。SBR 源码已入库但不编译。
- 关闭 `CONFIG_AUDIO_ENABLE_AAC`：组件注册为空、三个源文件不编译、分发/扫描/封面对应分支一起裁掉，
  `.m4a/.aac` 按"不支持格式"处理，便于一键回退。

## 6. 真机验证清单（QA）

1. ffmpeg 生成 32/44.1/48k × 单/双声道 `.m4a` 与 `.aac`，播放出声、时长/进度正确；
2. 连续 seek 到 0 / 中间 / 末尾；m4a 应精确到 sample，ADTS 误差 ≤ 1 帧；
3. 逐 512 帧读取不丢不重：播完无明显停顿/重复音；结尾自然停止无满幅噪声；
4. 坏曲：HE-AAC（AOT=5/29）、22.05kHz、截断帧、伪装成 `.m4a` 的 MP3 → 明确报错跳过，
   下一首正常播放；
5. 内嵌 JPEG 封面 m4a 正常显示；PNG covr 回退到默认图；`.npd` 优先级不变；
6. 开机 pldata v4→v5 只重扫一次，之后重启不再重扫；新增/删除 m4a 触发指纹重扫；
7. 息屏 80MHz 播放无爆音、无 `allocate_dma_buf` 失败、无 I2S 欠载；
8. `MEM: stk:` 观察 `mp3_play`（80KB）高水位与 heap/PSRAM 稳定（打印值是**剩余**字节）；
9. EQ 开/关、回放增益非 1.0、循环/随机/记忆播放、MP3/FLAC/OGG/WAV 回归。

> 当前状态：代码已完成并通过静态审查与主机级语法/冒烟验证；QA 仅搭建了隔离 IDF 6.1
> 环境并验证 `idf.py --version`，**尚未编译固件、尚未上真机**。编译与真机验证待用户在
> Windows IDF 6.1 + `idfc` 环境完成。
