set(tremor_srcs
    "src/block.c"
    "src/codebook.c"
    "src/floor0.c"
    "src/floor1.c"
    "src/info.c"
    "src/mapping0.c"
    "src/mdct.c"
    "src/registry.c"
    "src/res012.c"
    "src/sharedbook.c"
    "src/synthesis.c"
    "src/vorbisfile.c"
    "src/window.c")

idf_component_register(
    SRCS ${tremor_srcs}
    INCLUDE_DIRS "include"
    REQUIRES libogg)

# Tremor 是 OGG 播放的解码热点，固定 -O2 而不是 IDF 默认的 -Og/-Os；
# -fno-strict-aliasing 避免定点代码的类型双关在 -O2 下被误优化；
# -w 屏蔽第三方代码告警（与 minimp3/dr_flac 的处理一致）。
set_source_files_properties(${tremor_srcs} PROPERTIES
    COMPILE_OPTIONS "-O2;-fno-strict-aliasing;-w;-DHAVE_ALLOCA_H=1")
