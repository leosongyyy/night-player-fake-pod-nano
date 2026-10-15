# 把 default_cover_flash.c 里 RGB565 像素数据的两字节顺序翻转（高字节在前 → 本机小端）
#
# 背景：该素材是为 LVGL 8.4 + CONFIG_LV_COLOR_16_SWAP=y 生成的，像素按"高字节在前"
# 存放，面板直接收到这串字节。LVGL 9 移除了 LV_COLOR_16_SWAP，改为渲染成
# 本机（小端）RGB565 后在 flush 回调里整体交换一次；因此素材本身必须是本机字节序，
# 否则等于被交换两次。
#
# 用法: pwsh -NoProfile -File swap_cover_bytes.ps1 -Path <default_cover_flash.c>
param([Parameter(Mandatory = $true)][string]$Path)

$enc = New-Object System.Text.UTF8Encoding($false)   # UTF-8 无 BOM，与原文件一致
$text = [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)

$beginMarker = 'default_cover_flash_map[] = {'
$endMarker = '};'
$b = $text.IndexOf($beginMarker)
if ($b -lt 0) { throw "找不到数组起始标记 $beginMarker" }
$b = $b + $beginMarker.Length
$e = $text.IndexOf($endMarker, $b)
if ($e -lt 0) { throw "找不到数组结束标记" }

$head = $text.Substring(0, $b)
$body = $text.Substring($b, $e - $b)
$tail = $text.Substring($e)

# 统计 0xHH token 数量（必须为偶数才能两两配对）
$tokens = [regex]::Matches($body, '0x[0-9a-fA-F]{2}')
"token 数量: $($tokens.Count)"
if ($tokens.Count % 2 -ne 0) { throw "token 数量为奇数，无法两两配对" }

# 记录交换前的前几个 token 供校验
$before = ($tokens | Select-Object -First 6 | ForEach-Object { $_.Value }) -join ','
"交换前前 6 个: $before"

# 两两交换：非重叠、从左到右，逗号留原位（见注释说明配对语义）
$rx = [regex]'0x([0-9a-fA-F]{2})(\s*,\s*)0x([0-9a-fA-F]{2})'
$swapped = $rx.Replace($body, { param($m) "0x$($m.Groups[3].Value)$($m.Groups[2].Value)0x$($m.Groups[1].Value)" })
$afterTokens = [regex]::Matches($swapped, '0x[0-9a-fA-F]{2}')
"交换后 token 数量: $($afterTokens.Count)"
if ($afterTokens.Count -ne $tokens.Count) { throw "交换后 token 数量变化了" }
$after = ($afterTokens | Select-Object -First 6 | ForEach-Object { $_.Value }) -join ','
"交换后前 6 个: $after"

# 校验：交换后的第 k 个 token 应等于交换前的第 (k^1) 个
for ($i = 0; $i -lt $tokens.Count; $i++) {
    $expect = $tokens[$i -bxor 1].Value
    if ($afterTokens[$i].Value -ne $expect) {
        throw "第 $i 个 token 校验失败: 得到 $($afterTokens[$i].Value)，期望 $expect"
    }
}
"逐 token 校验通过（共 $($tokens.Count) 个）"

$out = $head + $swapped + $tail
[System.IO.File]::WriteAllText($Path, $out, $enc)
"已写回 $Path"
