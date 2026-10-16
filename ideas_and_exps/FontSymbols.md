# 内嵌字库字形清单（重新生成 system_global_font_lite.c / system_smaller_font.c）

> 2026-10-25 生成。**权威字符集文件：`ideas_and_exps/FontSymbols.symbols.txt`**（单行 383 字，
> 可直接喂给 `lv_font_conv --symbols`，不要在对话/文档里手工转抄——手抄漏过字）。
>
> 用途：TTF 主路径不可用时（font 分区为空/损坏、`CONFIG_UI_FONT_TTF=n`）界面会回退到**内嵌位图字体**，
> 这份清单保证回退态下 UI 文案不缺字。字体系统的整体设计见 `FontSystem.md`。

---

## 一、为什么要重做这份清单

现有两份内嵌字库是 TTF 迁移**之前**生成的，只覆盖 UI 现在用到的字的一小部分：

| 现有字库 | 字号 / 源字体 | 覆盖的不同字符数 |
| --- | --- | --- |
| `system_global_font_lite.c` | 24px / MiSans-Normal | **88** |
| `system_smaller_font.c` | 20px / MiSans-Bold | **91** |

而当前 `main/` 源码里**实际上屏**（会显示在界面上）的非 ASCII 字符有 **303 个** ——
**现有 lite 缺 218 个**。也就是说一旦回退到内嵌字库，界面上大约**三分之二的文案会变成方框**。
TTF 主路径掩盖了这个问题（FreeType 按需光栅化，字体里有什么就能显示什么）。

---

## 二、统计口径

1. 遍历 `main/` 下所有 `*.c` / `*.h`；
2. 先剥掉 `/* */` 块注释与 `//` 行注释（注释里的中文不上屏，不能算）；
3. 再剥掉整条 `ESP_LOGx(...)` / `LV_LOGx(...)` 调用（日志文案也不上屏）；
4. 从剩下的字符串字面量 `"..."` 里取出所有 **码点 > 127** 的字符并去重。

| 集合 | 字符数 |
| --- | --- |
| 源码字符串里的全部非 ASCII 字符（含日志文案） | 381 |
| 其中**只**出现在日志文案里、不上屏（可选剔除） | 78 |
| **UI 实际上屏用到** | **303** |
| 现有 `system_global_font_lite.c`（24px）覆盖 | 88 |
| 现有 `system_smaller_font.c`（20px）覆盖 | 91 |
| UI 用到、但现有 lite 缺失 | **218** |
| **最终清单 `FontSymbols.symbols.txt`（381 与旧字库取并集）** | **383** |

**为什么给 383 而不是 303**：采用超集策略，把"全部 381 个源码字符"与"旧字库里的字符"取并集，
宁可多带 78 个只用于日志的字（24px、2bpp 下每字约 144 B，合计约 **11 KB**，相对 7.9 MB 的
`factory` 分区可以忽略），也不冒"某条文案的字被漏掉、回退态下变方框"的风险。
（旧字库里还有 `！` 这类当前源码已不出现、但留着更稳的字符，并集也一并保住了。）

---

## 三、怎么用（生成命令）

参数**照抄**现有字库文件头 `Opts:` 行，只替换 `--symbols` 与 `-o`：

```powershell
# 24px 正文（覆盖 main/core/fonts/system_global_font_lite.c）
lv_font_conv --bpp 2 --size 24 --no-compress --stride 1 --align 1 `
  --font spiffs/system.ttf `
  --symbols "$((Get-Content -Raw ideas_and_exps/FontSymbols.symbols.txt).Trim())" `
  --range 32-127 --format lvgl -o main/core/fonts/system_global_font_lite.c

# 20px 小字（覆盖 main/core/fonts/system_smaller_font.c）
lv_font_conv --bpp 2 --size 20 --no-compress --stride 1 --align 1 `
  --font spiffs/system.ttf `
  --symbols "$((Get-Content -Raw ideas_and_exps/FontSymbols.symbols.txt).Trim())" `
  --range 32-127 --format lvgl -o main/core/fonts/system_smaller_font.c
```

- `--font` 用 **`spiffs/system.ttf`**（Droid Sans Fallback）：与 TTF 主路径同源，回退态与正常态的
  字面观感一致，也不必再去找 MiSans 原始文件。若想保留旧的 MiSans 观感，换成自己的
  `MiSans-Normal.ttf` / `MiSans-Bold.ttf` 即可（20px 原来用的是 **Bold**，Droid 只有常规字重）。
- `--range 32-127` 必须保留（ASCII：数字、拉丁字母、半角标点）。
- `--symbols` 一定要用 `.Trim()`（或手工粘贴时确认末尾没有多余换行/空格，否则会多出一个空白字形）。

---

## 四、注意事项

1. **`.fallback` 必须保住**：两份字库末尾都是 `.fallback = &lv_font_montserrat_24`（24px）/
   `.fallback = &lv_font_montserrat_20`（20px）。UI 里的 `LV_SYMBOL_*` 图标（⚠ 警告、SD 卡、
   OK/CLOSE 等）**不在这份清单里**，全靠这个 fallback 链渲染。生成后检查该字段是否仍在；
   丢了图标会全变方框。
2. **换字体源会改变回退态的行高**：老字库是 26（24px）/ 24（20px）；2026-10-25 用
   `system.ttf`（Droid Sans Fallback）重新生成后实测 **27（24px）/ 23（20px）**。
   影响面：`virtual_list` 用 `lv_font_get_line_height()` 动态算行高（自适应，无碍）；
   `settings_page.c` 的两行文字图标用 `text_line_space=-6` 收紧（2×23-6=40，48px 图标区仍放得下）。
3. **用户数据不在清单内**：歌曲名/目录名/歌手（可能含生僻字、日文）是运行时字符串，
   点阵字库覆盖不了——这正是 TTF 主路径存在的意义；回退态下这类文字仍会缺字，属已知限制。
   ⚠ 也正因如此，TTF 是**延迟加载**的：只有真正要进常规界面时才加载（`FontSystem.md` §9 第 8 条）；
   低电 / 无卡 / 待扫描这些分支全程用内嵌位图字库——那些界面只有固定文案，不缺字。
4. **生成后必须重新构建**才生效（字库是编译进固件的 `.rodata`，不是分区文件）。
5. **`-o` 的文件名决定生成的符号名**（2026-10-25 真实踩坑）：lv_font_conv 用输出文件的
   主文件名命名 `const lv_font_t` 变量（`-o foo.c` → `foo`）。那次 lite 的 `-o` 误写成
   `system_global_font.c`，于是文件里定义的是 `system_global_font`：既与 `.h` 声明的
   `system_global_font_lite` 不符（链接期 `undefined reference to 'system_global_font_lite'`），
   又与**完整版**字库的符号重名（两者同时编译会冲突）。生成后请核对文件末尾的
   `const lv_font_t <名字> = {` 与对应 `.h` 里的 `extern` 声明一致。
6. **验收方式**：临时把 `CONFIG_UI_FONT_TTF` 关掉（或在 `sdkconfig.defaults.debug` 里注掉）
   构建一次，把主界面、设置菜单、下拉控制中心、各 Toast 都点一遍，确认没有方框；
   验完再打开。这是唯一能覆盖"回退态"的实测方法。

---

## 五、以后文案变了怎么重新统计

把下面这段存成 .ps1 跑一遍即可（就是本文档生成时的脚本）：

```powershell
# 统计 main/ 下所有源码字符串字面量里的非 ASCII 字符，并与现有字库取并集
$old = @{}
foreach ($p in @(@('lite','main/core/fonts/system_global_font_lite.c'),
                 @('small','main/core/fonts/system_smaller_font.c'))) {
  $t = Get-Content -Raw -Encoding UTF8 $p[1]
  $old[$p[0]] = [regex]::Match($t, '--symbols\s+(.*?)\s+--range').Groups[1].Value
}
function Get-GlyphSet([bool]$stripLog) {
  $s = New-Object System.Collections.Generic.HashSet[char]
  foreach ($f in (Get-ChildItem -Path main -Recurse -Include *.c,*.h)) {
    $t = Get-Content -Raw -Encoding UTF8 $f.FullName
    $t = [regex]::Replace($t, '/\*.*?\*/', '', [System.Text.RegularExpressions.RegexOptions]::Singleline)
    $t = [regex]::Replace($t, '//[^\r\n]*', '')
    if ($stripLog) {   # 剔除整条日志调用（日志文案不上屏）
      $t = [regex]::Replace($t, '(ESP_LOG|LV_LOG)[A-Z]?\s*\([^;]*?\)\s*;', '',
                            [System.Text.RegularExpressions.RegexOptions]::Singleline)
    }
    foreach ($m in [regex]::Matches($t, '"((?:[^"\\]|\\.)*)"')) {
      foreach ($c in $m.Groups[1].Value.ToCharArray()) { if ([int]$c -gt 127) { [void]$s.Add($c) } }
    }
  }
  ,$s
}
$all = Get-GlyphSet $false          # 含日志文案
$ui  = Get-GlyphSet $true           # 仅 UI 上屏
$final = New-Object System.Collections.Generic.HashSet[char]
foreach ($c in $all)         { [void]$final.Add($c) }
foreach ($c in $old['lite'].ToCharArray())  { [void]$final.Add($c) }
foreach ($c in $old['small'].ToCharArray()) { [void]$final.Add($c) }

"全部=$($all.Count) UI上屏=$($ui.Count) 最终=$(@($final).Count)"
(-join (@($final) | Sort-Object)) | Set-Content -Path 'ideas_and_exps/FontSymbols.symbols.txt' -Encoding UTF8
```

⚠ 脚本只**读**源码、只**写**这份清单文件，不动任何源码（项目铁律：不要用 shell 文本工具改写源文件）。
运行环境：PowerShell 7（`pwsh`）；`-Encoding UTF8` 保证中文不损坏。

---

## 六、与本次"扫描期间文字变方框"的关系

2026-10-25 发现的 bug：扫描歌曲的**写入阶段**屏幕上的文字变成方框、日志刷字体加载失败。
结论是**扫描写 pldata 时 `esp_partition_erase/write` 会关闭外部内存缓存**，此时 PSRAM 不可访问——
而 LVGL 对象堆（`core/ui/lvgl_mem.c`）与 FreeType 的字形位图缓存都在 PSRAM，于是那一帧渲染停摆、
新字形取不到，就表现为方框。属于"关缓存陷阱"的又一实例（见 AGENTS.md）。

**已修复（2026-10-25）**，两条措施配合：

1. **写入前抢住 LVGL 锁、让界面定格**：扫描进度回调在 `PLAYLIST_BUILD_PHASE_WRITE`
   阶段 `lvgl_port_lock(-1)`，主动把"正在写入扫描数据..."这一帧渲染提交，之后整个写入
   期间 LVGL 冻结；写完 flash 由扫描任务 `lvgl_port_unlock()`。宁可定格，也不让渲染在
   "PSRAM 不可访问"的窗口里跑到不可恢复的状态。
2. **TTF 改为延迟加载**：只有真正要进常规界面时才加载 TTF；低电 / 无卡 / 待扫描分支全程
   用内嵌位图字体——扫描期间根本不持有 FreeType 字形缓存，也就没有"字形取不到"这回事。
   这正是"把内嵌字库补全"的价值所在。

**回退态本身是安全的**：内嵌位图字库在固件 `.rodata` 里、渲染不需要任何运行时分配。
详见 `FontSystem.md` §9 第 7、8 条。
