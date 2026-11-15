set(ogg_srcs
    "src/bitwise.c"
    "src/framing.c"
    "src/ogg_psram_alloc.c")

idf_component_register(
    SRCS ${ogg_srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES heap freertos)

# 第三方容器库：固定 -O2（IDF 默认可能是 -Og/-Os），并屏蔽其告警。
set_source_files_properties(${ogg_srcs} PROPERTIES
    COMPILE_OPTIONS "-O2;-fno-strict-aliasing;-w")
