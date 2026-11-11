# Generates the MobCam icons (blue tile + white video camera) for both apps:
#   app/assets/icon.png, android-icon-{background,foreground,monochrome}.png
#   pc/gui/mobcam.ico
# Same design as pc/gui/AppIcon.cs. Re-run after changing the design.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = Resolve-Path (Join-Path $PSScriptRoot '..')

$blueLight = [System.Drawing.Color]::FromArgb(255, 76, 141, 255)
$blueDark = [System.Drawing.Color]::FromArgb(255, 31, 87, 214)

function New-RoundRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
  $d = [Math]::Min($r * 2, [Math]::Min($w, $h))
  $p = New-Object System.Drawing.Drawing2D.GraphicsPath
  $p.AddArc($x, $y, $d, $d, 180, 90)
  $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
  $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
  $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
  $p.CloseFigure()
  return $p
}

# Camera glyph in unit coordinates (0..1 of the tile), scaled around the centre by $scale.
function Get-Glyph([float]$size, [float]$scale) {
  $t = { param($v) ((0.5 + ($v - 0.5) * $scale) * $size) }
  $path = New-Object System.Drawing.Drawing2D.GraphicsPath ([System.Drawing.Drawing2D.FillMode]::Alternate)
  $x0 = & $t 0.16; $y0 = & $t 0.31; $x1 = & $t 0.63; $y1 = & $t 0.69
  $path.AddPath((New-RoundRect $x0 $y0 ($x1 - $x0) ($y1 - $y0) (0.08 * $size * $scale)), $false)
  $path.AddPolygon([System.Drawing.PointF[]]@(
      (New-Object System.Drawing.PointF((& $t 0.65), (& $t 0.45))),
      (New-Object System.Drawing.PointF((& $t 0.85), (& $t 0.33))),
      (New-Object System.Drawing.PointF((& $t 0.85), (& $t 0.67))),
      (New-Object System.Drawing.PointF((& $t 0.65), (& $t 0.55)))))
  $lx = & $t 0.30; $ly = & $t 0.41; $ld = 0.18 * $size * $scale
  $path.AddEllipse($lx, $ly, $ld, $ld)  # alternate fill mode -> lens is a hole
  return $path
}

function New-Canvas([int]$size) {
  $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = 'AntiAlias'
  $g.Clear([System.Drawing.Color]::Transparent)
  return @($bmp, $g)
}

function Draw-Tile([int]$size, [bool]$rounded) {
  $bmp, $g = New-Canvas $size
  $rect = New-Object System.Drawing.RectangleF(0, 0, $size, $size)
  $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, $blueLight, $blueDark, 45.0)
  if ($rounded) { $g.FillPath($brush, (New-RoundRect 0 0 ($size - 1) ($size - 1) ($size * 0.22))) }
  else { $g.FillRectangle($brush, $rect) }
  return @($bmp, $g)
}

$assets = Join-Path $root 'app\assets'

# Legacy launcher icon: rounded tile with the glyph
$bmp, $g = Draw-Tile 1024 $true
$g.FillPath([System.Drawing.Brushes]::White, (Get-Glyph 1024 1.0))
$bmp.Save((Join-Path $assets 'icon.png')); $g.Dispose(); $bmp.Dispose()

# Adaptive icon layers (the launcher masks them; keep the glyph inside the 66% safe zone)
$bmp, $g = Draw-Tile 1024 $false
$bmp.Save((Join-Path $assets 'android-icon-background.png')); $g.Dispose(); $bmp.Dispose()
foreach ($name in 'android-icon-foreground.png', 'android-icon-monochrome.png') {
  $bmp, $g = New-Canvas 1024
  $g.FillPath([System.Drawing.Brushes]::White, (Get-Glyph 1024 0.62))
  $bmp.Save((Join-Path $assets $name)); $g.Dispose(); $bmp.Dispose()
}

# Windows .ico with PNG-compressed frames
$sizes = 16, 20, 24, 32, 40, 48, 64, 128, 256
$frames = foreach ($s in $sizes) {
  $bmp, $g = Draw-Tile $s $true
  $g.FillPath([System.Drawing.Brushes]::White, (Get-Glyph $s 1.0))
  $ms = New-Object System.IO.MemoryStream
  $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $bmp.Dispose()
  , $ms.ToArray()
}
$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $out
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
  $s = $sizes[$i]; $len = $frames[$i].Length
  $w.Write([byte]($(if ($s -ge 256) { 0 } else { $s }))); $w.Write([byte]($(if ($s -ge 256) { 0 } else { $s })))
  $w.Write([byte]0); $w.Write([byte]0); $w.Write([uint16]1); $w.Write([uint16]32)
  $w.Write([uint32]$len); $w.Write([uint32]$offset)
  $offset += $len
}
foreach ($f in $frames) { $w.Write($f) }
$w.Flush()
[System.IO.File]::WriteAllBytes((Join-Path $root 'pc\gui\mobcam.ico'), $out.ToArray())

Write-Host 'Icons written.'
