# libhelix-aac（vendored 第三方组件）

## 来源与版本

- 上游：`https://gitee.com/open-vela/external_libhelix-aac`（dev 分支），Helix DNA Technology 的 32 位定点 HE-AAC 解码器。
- 本目录内的 `include/*.h`、`src/*.c`、`src/*.s`、`readme.txt` 均为上游**原样 vendored**；每个文件的 RealNetworks RPSL/RCSL 版权头声明一字未改、未删。
- `LICENSE.RPSL` 为 RealNetworks Public Source License Version 1.0（Rev. 2002-10-28）的真实全文，取自 `https://github.com/Microchip-MPLAB-Harmony/helix_mp3/blob/master/fixpnt/RPSL.txt`。

## 许可证（重要）

- 本组件为 **RPSL-1.0 / RCSL-1.0 双许可**，**不是 Apache-2.0**。
- 项目主体为 MIT；本目录作为**许可证例外**声明：MIT 不覆盖 `components/libhelix-aac/` 下的上游代码。
- 商业分发、闭源产品集成以及 AAC 编解码专利授权需要由分发方自行评估/取得 RealNetworks 与相关专利方的许可；本仓库不对专利授权作任何保证。
- RCSL 全文未公开，本目录只入库可公开获取的 RPSL 全文；若你持有有效 RCSL，可按其条款使用本组件。

## 本工程的裁剪

- 只编译 AAC-LC 需要的核心源文件；SBR/PS 相关源文件虽已入库但**不编译**（`HELIX_FEATURE_AUDIO_CODEC_AAC_SBR` 未定义）。
- 不支持 HE-AAC（AOT=5/29）、MAIN、SSR、LTP；容器层在 `AudioSpecificConfig` / ADTS 头阶段就拒绝非 LC 配置。
- 解码器状态通过 `AACInitDecoderPre()` 由调用方（`aac_decoder.c`）在 PSRAM 中预分配，**不调用 `AACFreeDecoder()`**（该接口会 `free()` 掉调用方 arena 内的指针）；关闭时由调用方直接释放整块 PSRAM。
- 仅使用 Helix 的 C API：`AACInitDecoderPre` / `AACSetFormat` / `AACSetRawBlockParams` / `AACDecode` / `AACGetLastFrameInfo` / `AACFlushCodec`。

## 构建开关

根 `CMakeLists.txt` 的 `COMPONENTS` 白名单已加入 `libhelix-aac`；`main/Kconfig.projbuild` 的 `CONFIG_AUDIO_ENABLE_AAC` 控制上层 AAC 解码器是否编入固件。
