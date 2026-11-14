#ifndef __CONFIG_TYPES_H__
#define __CONFIG_TYPES_H__

/* 与 Tremor 的 config_types.h 保持同一套底层类型：在 riscv32 上 int32_t
   是 long int，而 Tremor 内部大量使用 int*，用 stdint 会让两者指针类型
   不兼容（-Werror=incompatible-pointer-types）。 */
typedef short ogg_int16_t;
typedef unsigned short ogg_uint16_t;
typedef int ogg_int32_t;
typedef unsigned int ogg_uint32_t;
typedef long long ogg_int64_t;
typedef unsigned long long ogg_uint64_t;

#endif
