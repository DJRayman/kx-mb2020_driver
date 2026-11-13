# SPDX-License-Identifier: GPL-2.0-or-later
param([string]$R = (Join-Path $PSScriptRoot 'out'), [string]$PcIp = '192.168.1.10')   # $R: папка с print1.prn, туда же пишутся результаты
$ErrorActionPreference = 'Stop'
$P = $PSScriptRoot
Add-Type -Path "$P\Jbig1.cs","$P\GdiPrn.cs" -ReferencedAssemblies System.IO.Compression
Add-Type -AssemblyName System.Drawing
$log = New-Object Collections.Generic.List[string]

# 1) декодирование print1.prn
$d = [IO.File]::ReadAllBytes("$R\print1.prn")
$page = [GdiPrn]::DecodePage($d, $log)
[GdiPrn]::SavePng1($page, "$R\print1.png")
$log.Add("print1.png: $($page.W)x$($page.H)")

# 2) обратная проверка: перекодировать каждую полосу нашим кодером и сравнить с оригиналом байт в байт
$i = 0
foreach ($b in [GdiPrn]::ParseBands($d)) { $i++
  $im = [Jbig1]::Decode($b.Bie, 0, $b.Bie.Length)
  $re = [Jbig1]::Encode($im, [Jbig1]::LRLTWO -bor [Jbig1]::TPBON, $true)
  $same = ($re.Length -eq $b.Bie.Length) -and (-not (Compare-Object $re $b.Bie -SyncWindow 0))
  $verdict = if ($same) { 'СОВПАДАЕТ' } else { 'отличается' }
  $log.Add("полоса $i : перекодирование $($re.Length) байт vs оригинал $($b.Bie.Length) байт -> $verdict") }

# 3) своя страница: рамка 1 см от края листа + текст, 600 dpi, растр 4760x6814 с началом в (4.23 мм, 4.23 мм)
$W = 4760; $H = 6814
$bmp = New-Object Drawing.Bitmap $W, $H, ([Drawing.Imaging.PixelFormat]::Format24bppRgb)
$bmp.SetResolution(600, 600)
$g = [Drawing.Graphics]::FromImage($bmp); $g.Clear([Drawing.Color]::White)
$g.PageUnit = [Drawing.GraphicsUnit]::Millimeter
$g.TextRenderingHint = [Drawing.Text.TextRenderingHint]::SingleBitPerPixelGridFit
$hm = 4.23
$pen = New-Object Drawing.Pen ([Drawing.Color]::Black), 0.5
$g.DrawRectangle($pen, [single](10 - $hm), [single](10 - $hm), [single]190, [single]277)
$f = New-Object Drawing.Font 'Arial', 36, ([Drawing.FontStyle]::Bold), ([Drawing.GraphicsUnit]::Point)
$g.DrawString('OWN DRIVER TEST', $f, [Drawing.Brushes]::Black, [single](20 - $hm), [single](20 - $hm))
$g.Dispose()
$own = New-Object Jbig1+Image $W, $H
$bd = $bmp.LockBits((New-Object Drawing.Rectangle 0, 0, $W, $H), 'ReadOnly', $bmp.PixelFormat)
$row = New-Object byte[] ($bd.Stride)
for ($y = 0; $y -lt $H; $y++) {
  [Runtime.InteropServices.Marshal]::Copy([IntPtr]($bd.Scan0.ToInt64() + $y * $bd.Stride), $row, 0, $bd.Stride)
  $o = $y * $own.Bpl
  for ($x = 0; $x -lt $W; $x++) { if ($row[$x * 3] -lt 128) { $own.Bits[$o + ($x -shr 3)] = $own.Bits[$o + ($x -shr 3)] -bor (0x80 -shr ($x -band 7)) } } }
$bmp.UnlockBits($bd); $bmp.Dispose()
$job = [GdiPrn]::BuildJob($own, 'OWN DRIVER TEST', $env:USERNAME, $env:COMPUTERNAME, $PcIp, (Get-Date))
[IO.File]::WriteAllBytes("$R\print-own.prn", $job)
$log.Add("print-own.prn: $($job.Length) байт")

# 4) проверка: свой поток декодируется обратно в ту же картинку
$log2 = New-Object Collections.Generic.List[string]
$back = [GdiPrn]::DecodePage($job, $log2)
$eq = -not (Compare-Object $back.Bits $own.Bits -SyncWindow 0)
$log.Add("print-own: обратное декодирование $($back.W)x$($back.H), совпадает с исходным растром: $eq")
$log2 | % { $log.Add('  own ' + $_) }
[GdiPrn]::SavePng1($back, "$R\print-own.png")
$log | Out-File "$R\12-jbig-roundtrip.txt" -Encoding utf8
$log
