# SPDX-License-Identifier: GPL-2.0-or-later
param([string]$Printer = 'Panasonic KX-MB2020RU', [string]$Text = 'KX-MB2020 TEST 1')
Add-Type -AssemblyName System.Drawing
$pd = New-Object Drawing.Printing.PrintDocument
$pd.PrinterSettings.PrinterName = $Printer
$pd.DocumentName = $Text
$a4 = $pd.PrinterSettings.PaperSizes | ?{ $_.Kind -eq 'A4' } | select -First 1
if ($a4) { $pd.DefaultPageSettings.PaperSize = $a4 }
$pd.DefaultPageSettings.Margins = New-Object Drawing.Printing.Margins 0,0,0,0
$ps = $pd.DefaultPageSettings
"Printer=$($pd.PrinterSettings.PrinterName) Valid=$($pd.PrinterSettings.IsValid) Paper=$($ps.PaperSize.PaperName) $($ps.PaperSize.Width)x$($ps.PaperSize.Height) (1/100in) Res=$($ps.PrinterResolution.X)x$($ps.PrinterResolution.Y) Kind=$($ps.PrinterResolution.Kind) Color=$($ps.Color) Source=$($ps.PaperSource.SourceName) HardMargin=$($ps.HardMarginX),$($ps.HardMarginY)"
$pd.add_PrintPage({
  param($s, $e)
  $g = $e.Graphics
  $g.PageUnit = [Drawing.GraphicsUnit]::Millimeter
  $hx = $e.PageSettings.HardMarginX * 0.254; $hy = $e.PageSettings.HardMarginY * 0.254   # 1/100" -> мм
  $pen = New-Object Drawing.Pen ([Drawing.Color]::Black), 0.5
  $g.DrawRectangle($pen, [single](10 - $hx), [single](10 - $hy), [single]190, [single]277)
  $f = New-Object Drawing.Font 'Arial', 36, ([Drawing.FontStyle]::Bold), ([Drawing.GraphicsUnit]::Point)
  $g.DrawString($Text, $f, [Drawing.Brushes]::Black, [single](20 - $hx), [single](20 - $hy))
  $e.HasMorePages = $false
})
$pd.Print()
"sent at $(Get-Date -Format s)"
