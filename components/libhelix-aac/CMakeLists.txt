# ============================================================================
# libhelix-aac —— RealNetworks 定点 HE-AAC 解码器（vendored 本地组件）
# ----------------------------------------------------------------------------
# 来源：https://gitee.com/open-vela/external_libhelix-aac （dev 分支），
#       上游为 Helix DNA Technology 的 fixpt AAC 解码器。
# 许可证：RPSL-1.0 / RCSL-1.0 双许可，全文见 LICENSE.RPSL；每个 vendored
#         源文件的 RealNetworks 版权头均原样保留，未做任何删改。
#         **本组件不是 Apache-2.0，也不在项目的 MIT 许可覆盖范围内**。
#
# 只编译 AAC-LC 需要的核心源文件；SBR/PS 相关源文件已随上游一并入库以保留
# 完整源码，但不参与编译（HELIX_FEATURE_AUDIO_CODEC_AAC_SBR 未定义，
# AAC_ENABLE_SBR 不会打开）。HE-AAC（AOT=5/29）在容器层被拒绝。
#
# 与 Tremor 组件一致：固定 -O2 保证解码实时性，-fno-strict-aliasing 防止
# 定点代码的类型双关被误优化，-w 屏蔽第三方老代码告警（IDF 默认 -Werror）。
# -DCONFIG_LIB_HELIX_AAC_PNS=1 打开 AAC-LC 的 PNS 工具（pns.c 内的实现
# 由该宏保护；不定义会 undefined reference to raac_PNS）。
# ============================================================================

# Kconfig 关闭 AAC 时注册成空组件（不编译任何 Helix 源文件、不链接解码代码）
if(NOT CONFIG_AUDIO_ENABLE_AAC)
    idf_component_register()
    return()
endif()

set(helix_aac_srcs
    "src/aacdec.c"
    "src/aactabs.c"
    "src/bitstream.c"
    "src/buffers.c"
    "src/dct4.c"
    "src/decelmnt.c"
    "src/dequant.c"
    "src/fft.c"
    "src/filefmt.c"
    "src/huffman.c"
    "src/hufftabs.c"
    "src/imdct.c"
    "src/noiseless.c"
    "src/pns.c"
    "src/stproc.c"
    "src/tns.c"
    "src/trigtabs.c")

idf_component_register(
    SRCS ${helix_aac_srcs}
    INCLUDE_DIRS "include")

# USE_DEFAULT_STDLIB 必须在命令行定义：buffers.c 在包含 coder.h（间接定义它）
# 之前就判断该宏，不定义会去找上游 Helix 的 hlxclib/stdlib.h（本仓库没有）。
set_source_files_properties(${helix_aac_srcs} PROPERTIES
    COMPILE_OPTIONS "-O2;-fno-strict-aliasing;-w;-DCONFIG_LIB_HELIX_AAC_PNS=1;-DUSE_DEFAULT_STDLIB=1")
