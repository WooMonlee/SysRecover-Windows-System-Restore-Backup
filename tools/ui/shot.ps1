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
}
'@
if (-not ('UiShot' -as [type])) { Add-Type -TypeDefinition $sig }

Get-Process SysRecoverUI -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
$p = Start-Process -FilePath $Exe -PassThru

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

$bmp = New-Object System.Drawing.Bitmap($w, $ht)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$size = New-Object System.Drawing.Size($w, $ht)
$g.CopyFromScreen($r.Left, $r.Top, 0, 0, $size)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()

Write-Output ("OK {0} {1}x{2} at {3},{4}" -f $Out, $w, $ht, $r.Left, $r.Top)
