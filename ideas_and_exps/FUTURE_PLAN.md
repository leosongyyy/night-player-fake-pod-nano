# 未来计划

> 本文档用于存放想法、Bug分析记录和和未来计划  
> **只有本文档（`FUTURE_PLAN.md`）里出现过的内容，才可以写进 `DONE_PLANS.md` 中**——
> 未在本文件登记过的临时改动/额外发挥，不做记录。若遇到踩坑，可以单独另起一个文档记录。
> 已完成的内容从本文档移入隔壁 `DONE_PLANS.md`，同一会话中所作的对同一个功能点的变更应该合并在一条记录里。

## 23. 歌词显示

还没想好

## 34. 在Token预算充足时，调查谁吃了IRAM，并尽量把LVGL的缓冲区优化回以前的1/10 VRES的程度。

> 🆕 **2026-09-16：本条现在有了现成的调查工具**——`idfc.exe size` 可打印固件体积分布表；
> `idfc.exe size --detailed` 输出精确到 `.obj` 的树形分析（同时落盘到 `build/<variant>/size_analysis_report.txt`）。
> 实测 debug 版基线：**`IRAM 100.0%（16384/16384，余 0）`、`DIRAM 47.06%（余 180,933 字节）`**。
> ⚠ 解读时注意：**IRAM 被填满是 ESP32-S3 的正常现象**（.text/.vectors 本就会用满），
> 真正该盯的是 **DIRAM 余量**与 **`MALLOC_CAP_INTERNAL` 的最大连续块**（见下文验收标准）。
> 配合串口的 `MEMTRACE:` 逐阶段插桩，可以定位到"是哪一阶段吃掉的"。

背景（2026-09 实测记录，详见 `PSRAM_TASK_STACK.md` 第 4 节）：
- 虽然v0.9.0.0_alpha3 能正常且稳定地运行，但是是关闭了堆毒化后卡着极限在运行。
- 现状：`EXAMPLE_LVGL_BUF_HEIGHT = EXAMPLE_LCD_V_RES / 12`（460/12=38 行 → 每块
  460×38×2B ≈ 34KB，双缓冲 ≈ 68KB **DMA**），帧率接近原 30fps，DMA 稳态余量约 15KB。
- 为什么从 1/10 降下来：原 `V_RES/10`（双缓冲 ≈ 83KB DMA）会把 DMA 池压到不足 3KB，
  SD 卡每次读的 512B `allocate_dma_buf` 都分配不出来 → 解码器从未启动 → 被
  `mp3_player` 判为"数据损坏" → 坏曲自动跳过。用户侧现象是"开机先播一秒上次的歌，
  然后跳到别的队列去"。所以这是**内存不足的连锁反应**，不是播放/随机逻辑的 bug。
- ⚠ **2026-09-10 实测修正，详见 `IRAM_DMA_AUDIT.md`**：上面"开机流程吃掉 126KB DMA"的
  归因**是错的**。用新增的 `core/utils/mem_trace` 逐阶段插桩（debug 与 releasecandidate 各跑
  一轮完整启动 + 切歌）得到的结论：
  - **封面加载不占内部 RAM，更不占 DMA**：`.npd` 文件缓冲 61~77KB、JPEG 数据副本 61~77KB、
    解码像素 423200B **全部落 PSRAM**（用 `esp_ptr_external_ram()` 实测落点确认）。
    整条链路在内部 RAM 上只有 ~3~4KB 临时开销（JPEG 的 3100B work buffer 等），切歌即释放。
    JPEG 解码确实**不需要 DMA**——那个工作缓冲用的是 `MALLOC_CAP_DEFAULT`。
  - **真凶是 `app_ui_create()` 的 LVGL 控件对象**：`LV_MEM_CUSTOM=stdlib` + 阈值
    `SPIRAM_MALLOC_ALWAYSINTERNAL=16384` 让每笔 ≤16KB 的 `malloc` 都**强制落内部 RAM**，
    实测这一段吃掉 **105KB 内部 RAM、新增 1795 个分配块**（其中 `settings_menu` 最大，约 55KB）。
    ⚠ **2026-09 二次实测修正**：按"关掉某一项前后直接对比"重测，`app_ui_create` 合计
    约 **80KB**，其中 `settings_menu` 只有 **约 29KB**——"55KB"是按残差估算偏大
    （见 `DONE_PLANS.md` 第 44 点）。
    此前"开机流程 126KB"其实是这份 UI 对象开销——只不过封面加载同期发生，被误记成了凶手。
  - **debug 与 releasecandidate 相差 18~28KB 内部 RAM，唯一配置差异是
    `CONFIG_HEAP_POISONING_COMPREHENSIVE=y`**（RC 独有，debug 早在 2026-09 已关）。
    这就是"debug 从未出现、只有 RC/release 出现"的真正原因——不是版本差异而是配置差异。
  - **RC + V_RES/10 已复现故障**：`app_ui_create` 结束时 DMA 只剩 **955B / 最大连续块 80B**
    → `sdmmc_cmd: allocate_dma_buf: not enough mem` → 读曲失败 → 判坏曲 → **开机跳曲**；
    同刻内部 RAM 最大连续块只有 **2804B**，而 `sleep_timer` 需要 4KB 内部栈
    → **睡眠定时器服务起不来**。
- 修复优先级（按性价比）：① RC 关闭堆毒化（+18~28KB，零风险）；✅ ② **已完成（2026-09）**：
  懒创建 `settings_menu`【实测 +29KB 内部 RAM/DMA、主界面提前 305ms；做法是把
  `lvgl_port_init` 提前 + 开机封面 + 设置菜单分级懒加载，见 `DONE_PLANS.md` 第 44 点】，
  `notification_panel`（约 8KB）未做、收益小；③ 调低
  `SPIRAM_MALLOC_ALWAYSINTERNAL` 让 LVGL 小对象落 PSRAM（+80~100KB，需实测滚动帧率）；
  ④ LVGL 自定义分配器按大小分流。
- IRAM 侧仍待查的对象：LVGL 堆（`LV_MEM_CUSTOM=stdlib` + 几百个常驻控件/样式/缓存）、
  `SPIRAM_MALLOC_ALWAYSINTERNAL=16384` 这个阈值（<16KB 的分配优先走内部 RAM）、
  以及各驱动的常驻缓冲。`mem_monitor` 的 `iram=` 列与
  `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)` 是第一手指标。
  此前在OtoClock上曾经研究出过给LVGL自定义内存分配的方案，或许可以把控件相关扔到PSRAM里去？（测测性能）
- 验收标准建议：绘制缓冲回到 `V_RES/10`（≈83KB DMA）后，DMA 稳态余量仍 ≥ 8~10KB、
  SD 卡读写无 `allocate_dma_buf` 失败、列表滚动帧率回到 30fps；
  **另加一条**：`MALLOC_CAP_INTERNAL` 的最大连续块 ≥ 4KB（SD 读要 512B、内部任务栈要 4KB，
  只看 free 不够，`largest` 才是能否分配成功的判据）。
- ⚠ 硬性约束：绘制缓冲**必须** `MALLOC_CAP_DMA`（非 DMA 会触发 SPI bounce 缓冲分配，
  失败则刷屏阻塞 → 任务看门狗复位）；调这个值前必须同时看帧率与
  `heap_caps_get_free_size(MALLOC_CAP_DMA)`。
  （当然，绘制缓冲区必须在IRAM里，不然带宽就不够刷满30帧了）






