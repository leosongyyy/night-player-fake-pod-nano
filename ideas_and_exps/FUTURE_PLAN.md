# 未来计划

> 本文档用于存放想法、Bug分析记录和和未来计划  
> **只有本文档（`FUTURE_PLAN.md`）里出现过的内容，才可以写进 `DONE_PLANS.md` 中**——
> 未在本文件登记过的临时改动/额外发挥，不做记录。若遇到踩坑，可以单独另起一个文档记录。
> 已完成的内容从本文档移入隔壁 `DONE_PLANS.md`，同一会话中所作的对同一个功能点的变更应该合并在一条记录里。

## 23. 歌词显示

还没想好

## 55. 扫描任务栈单位 bug：`BOOT_SCAN_STACK_BYTES / 4u`（fork 已修，master 未修）

**来源**：2026-11-15 评估 sakuraio fork（`ideas_and_exps/SAKURAIO_FORK_EVAL.md` §8.4）。

- **现象**：`main/ui/boot_flow.c` 的 `boot_show_scanning()` 里
  `xTaskCreatePinnedToCore(boot_scan_task, "boot_scan", BOOT_SCAN_STACK_BYTES / 4u, …)`，
  而 `BOOT_SCAN_STACK_BYTES = 32768` —— 注释写"32KB 栈"，**实际只给了 8KB**。
- **根因**：`/4` 是 FreeRTOS 旧 word 语义的残留。ESP-IDF 的 `xTaskCreatePinnedToCore`
  栈单位是**字节**（`usStackDepth` = size in bytes），不需要除。
- **为什么一直没炸**：扫描内核是**显式栈迭代**（`s_stack` 在 PSRAM），8KB 只装
  `entry_path[]`、`FILINFO` 等局部量；fork 的注释与主分支历史都表明 1233 首的扁平
  目录在 8KB 下能扫完，所以这个 bug 一直隐身（⚠ 这一条是据注释/历史推断，
  真要下调到 8KB 前应在 5000+ 首库里实测一次）。
- **fork 侧**：按注释意图传满 32KB，并加了 16KB 回退。方向对（修对了单位），
  但**尺寸不该跟着搬**——32KB 连续内部 RAM 在开机中后期本来就难拿，
  而且它自己注释也写了"历史 8KB 版本已能完成 1233 首扫描"。
- **落地做法**：只取单位修正，栈尺寸放到 **8~12KB**（12KB 给指纹/长路径留余量），
  并在创建失败时打印 `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)` 便于定位。

## 56. 扫描期动态分配改为优先 PSRAM（大目录内部 RAM 峰值）

**来源**：同上（`SAKURAIO_FORK_EVAL.md` §8.1~8.3）。

- **现状**：`main/core/storage/playlist_builder.c` 里每个目录条目的名字用
  `strdup()`（`collect_dir()` 三处：files / pls / subs），主入口另有
  `calloc(PLS_MAX_ENTRIES=4096) × 3` 的指针数组。`strdup`/`calloc` 走默认 malloc，
  在 `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` 下**优先吃内部 RAM**。
- **代价**：1233 首的扁平目录按 40~60B/条估算 ≈ **50~70KB 内部 RAM**峰值；
  fork 实测（32KB 栈版）扫 9042 首全库时，内部 RAM 被吃到只剩 **19 B**
  （连 `mem_monitor` 都起不来）。它之所以没崩，只是因为
  `strdup` 走 `MALLOC_CAP_DEFAULT`、内部不够时会**溢出到 PSRAM**；而
  `pvPortMalloc`（互斥锁/任务栈/TCB）固定 `MALLOC_CAP_INTERNAL|8BIT`，内部见底即失败
  ——"开机自动扫描"这一撞法在 fork 上就是 `xSemaphoreCreateMutex()` 拿到 NULL 的 assert。
- **为什么当年这么写**：S3 时代 PSRAM 只有 8MB 且带宽/延迟明显吃亏，
  扫大库时 PSRAM 本身也紧张，于是取舍成"先吃内部 RAM"。**S31 上这个前提反了**：
  16MB PSRAM（200MHz octal）+ 内部 RAM 才是稀缺资源（光蓝牙就吃掉约 50KB），
  扫描期间把名字塞进内部 RAM 属于把钱花在刀刃反面。
- **落地做法**：
  1. 加一个 `psram_strdup()` 辅助（`heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM)` +
     memcpy，失败回退 `strdup`），替换 `collect_dir()` 三处 `strdup`；
  2. `calloc(cap)` 三个数组改 `heap_caps_calloc(..., MALLOC_CAP_SPIRAM)`，
     或干脆按实际条目数 `realloc` 增长（4096×3×8 = 96KB 常驻）；
  3. 扫描前做准入检查：`heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < N`
     就延后/提示，别等 32KB 栈创建失败才回退。
  ⚠ 这些名字数组只在扫描任务里用，放 PSRAM 安全；但注意两个既有顺序：
  ① 每目录的名字字符串在**进入写 pldata 之前**就逐目录释放（`playlist_build_core` 的循环末尾），
  ② 三个指针数组本身是 `playlist_store_save_playlist()` **之后**才 `free` 的
  （`playlist_builder.c` 约 463 行）。写分区期间它们只是"挂着"、不会被解引用，
  所以搬 PSRAM 不影响关缓存窗口；改动时别在写入阶段去碰它们即可。

