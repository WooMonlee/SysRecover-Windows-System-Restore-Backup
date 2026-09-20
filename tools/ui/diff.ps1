# diff.ps1 - pixel-by-pixel compare of two PNGs: prints diff ratio + a heat map.
#
# IMPORTANT: keep this file ASCII-only (PowerShell 5.1 parses .ps1 as ANSI).
#
# usage: powershell -ExecutionPolicy Bypass -File tools\ui\diff.ps1
#        powershell -ExecutionPolicy Bypass -File tools\ui\diff.ps1 -A D:\ui-work\old1.png -B D:\ui-work\shot.png -Out D:\ui-work\heat.png
# A pixel counts as different when |dR|+|dG|+|dB| > Tol (antialiasing noise stays below).
param(
    [string]$A = 'D:\ui-work\old1.png',
    [string]$B = 'D:\ui-work\shot.png',
    [string]$Out = 'D:\ui-work\heat.png',
    [int]$Tol = 30
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$ia = [System.Drawing.Bitmap]::FromFile($A)
$ib = [System.Drawing.Bitmap]::FromFile($B)
$w = [Math]::Min($ia.Width, $ib.Width)
$h = [Math]::Min($ia.Height, $ib.Height)
Write-Output ("A {0} {1}x{2}   B {3} {4}x{5}" -f $A, $ia.Width, $ia.Height, $B, $ib.Width, $ib.Height)

$heat = New-Object System.Drawing.Bitmap($w, $h)
$diff = 0
$tot = 0
$minX = $w; $maxX = -1; $minY = $h; $maxY = -1
# per-row diff counts (printed sparsely) to locate structural differences
$rowTxt = ''
for ($y = 0; $y -lt $h; $y++) {
    $rowDiff = 0
    for ($x = 0; $x -lt $w; $x++) {
        $ca = $ia.GetPixel($x, $y)
        $cb = $ib.GetPixel($x, $y)
        $d = [Math]::Abs($ca.R - $cb.R) + [Math]::Abs($ca.G - $cb.G) + [Math]::Abs($ca.B - $cb.B)
        $tot++
        if ($d -gt $Tol) {
            $heat.SetPixel($x, $y, [System.Drawing.Color]::FromArgb(255, 255, 0, 0))
            $diff++
            $rowDiff++
            if ($x -lt $minX) { $minX = $x }
            if ($x -gt $maxX) { $maxX = $x }
            if ($y -lt $minY) { $minY = $y }
            if ($y -gt $maxY) { $maxY = $y }
        } else {
            $heat.SetPixel($x, $y, [System.Drawing.Color]::FromArgb(255, 240, 240, 240))
        }
    }
    if ($rowDiff -gt 60) { $rowTxt += (" y" + $y + ":" + $rowDiff) }
}
$heat.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$heat.Dispose(); $ia.Dispose(); $ib.Dispose()

if ($diff -gt 0) {
    Write-Output ("bbox x{0}..{1} y{2}..{3}" -f $minX, $maxX, $minY, $maxY)
}
Write-Output ("rows with >60 diffpx:" + $rowTxt)
Write-Output ("diff {0}/{1} = {2:N2}%  heat={3}" -f $diff, $tot, (100.0 * $diff / $tot), $Out)
