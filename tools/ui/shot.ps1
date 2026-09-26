# shot.ps1 - launch SysRecoverUI.exe and capture its window rect to a PNG.
#
# IMPORTANT: keep this file ASCII-only. PowerShell 5.1 parses .ps1 as ANSI when
# there is no BOM, so any CJK text in here breaks the parser.
#
# usage: powershell -ExecutionPolicy Bypass -File tools\ui\shot.ps1
#        powershell -ExecutionPolicy Bypass -File tools\ui\shot.ps1 -Out D:\ui-work\shot150.png
param(
    [string]$Exe = 'D:\Program Files (x86)\SysRecover\dist\SysRecoverUI.exe',
    [string]$Out = 'D:\ui-work\shot.png',
    [int]$WaitMs = 2500
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$sig = @'
using System;
using System.Runtime.InteropServices;
public class UiShot {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr insertAfter, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
}
'@
if (-not ('UiShot' -as [type])) { Add-Type -TypeDefinition $sig }

Get-Process SysRecoverUI -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
$p = Start-Process -FilePath $Exe -PassThru

# Plan-D caveat: the dist-root exe is the x86 launcher; on x64 it re-execs
# dist\x64\SysRecoverUI.exe and the parent exits immediately. If that happened,
# follow the real (child) process for the window polling below.
if ($p.HasExited) {
    Start-Sleep -Milliseconds 500
    $child = Get-Process SysRecoverUI -ErrorAction SilentlyContinue | Sort-Object StartTime -Descending | Select-Object -First 1
    if ($child) { $p = $child }
}

# Poll for the window (MainWindowHandle is more reliable than FindWindow here;
# window class is SysRecoverUI, see CMainForm::GetWindowClassName()).
$h = [IntPtr]::Zero
for ($i = 0; $i -lt ($WaitMs * 2); $i += 200) {
    Start-Sleep -Milliseconds 200
    if ($p.HasExited) { break }
    $p.Refresh()
    if ($p.MainWindowHandle -ne [IntPtr]::Zero) { $h = $p.MainWindowHandle; break }
}
if ($h -eq [IntPtr]::Zero) { Write-Output 'ERR: window not found'; exit 1 }
[void][UiShot]::ShowWindow($h, 5)          # SW_SHOW
[void][UiShot]::SetForegroundWindow($h)
Start-Sleep -Milliseconds 500

$r = New-Object 'UiShot+RECT'
[void][UiShot]::GetWindowRect($h, [ref]$r)
$w = $r.Right - $r.Left
$ht = $r.Bottom - $r.Top

# Bring the window on top (it may be occluded by the console; SetForegroundWindow
# alone can fail due to foreground-lock rules), then capture the window itself via
# PrintWindow (PW_RENDERFULLCONTENT=2) so an occluded window still yields its own
# pixels. If PrintWindow comes back empty (rare for GDI apps), fall back to a
# screen grab of the window rect (old behaviour).
[void][UiShot]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, 0x0045)   # HWND_TOPMOST + NOMOVE|NOSIZE|SHOWWINDOW
[void][UiShot]::SetForegroundWindow($h)
Start-Sleep -Milliseconds 500

$r = New-Object 'UiShot+RECT'
[void][UiShot]::GetWindowRect($h, [ref]$r)
$w = $r.Right - $r.Left
$ht = $r.Bottom - $r.Top

$bmp = New-Object System.Drawing.Bitmap($w, $ht)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$blank = $true
$hdc = $g.GetHdc()
$pw = [UiShot]::PrintWindow($h, $hdc, 2)
$g.ReleaseHdc($hdc)
if ($pw) {
    # grid-sample: if every sample is transparent or pure black, treat as empty
    $nonEmpty = 0
    for ($gy = 5; $gy -lt $ht -and $nonEmpty -eq 0; $gy += [Math]::Max(1, [int]($ht / 12))) {
        for ($gx = 5; $gx -lt $w; $gx += [Math]::Max(1, [int]($w / 12))) {
            $c = $bmp.GetPixel($gx, $gy)
            if (($c.R + $c.G + $c.B) -gt 0) { $nonEmpty = 1; break }
        }
    }
    if ($nonEmpty -eq 1) { $blank = $false }
}
if ($blank) {
    $size = New-Object System.Drawing.Size($w, $ht)
    $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $size)
}
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()

Write-Output ("OK {0} {1}x{2} at {3},{4} mode={5}" -f $Out, $w, $ht, $r.Left, $r.Top, $(if ($blank) { 'screen' } else { 'printwindow' }))
