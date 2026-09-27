# shot.ps1 - launch SysRecoverUI.exe and capture its window rect to a PNG.
#
# IMPORTANT: keep this file ASCII-only. PowerShell 5.1 parses .ps1 as ANSI when
# there is no BOM, so any CJK text in here breaks the parser.
#
# usage: powershell -ExecutionPolicy Bypass -File tools\ui\shot.ps1
#        powershell -ExecutionPolicy Bypass -File tools\ui\shot.ps1 -Out D:\ui-work\shot150.png
#        powershell -ExecutionPolicy Bypass -File tools\ui\shot.ps1 -Lang en -Out D:\ui-work\en.png
#        powershell -ExecutionPolicy Bypass -File tools\ui\shot.ps1 -Click ModeBackup -Out D:\ui-work\bk.png
#        powershell -ExecutionPolicy Bypass -File tools\ui\shot.ps1 -Hover RepairBootBtn -Out D:\ui-work\tip.png
#
# -Lang  sets SYSRECOVER_LANG for the child process (i18n smoke / bilingual review).
# -Click posts WM_LBUTTONDOWN/UP at the centre of a control (see skin/main.xml
#        pos=); used for interaction shots (tab switch, etc).
# -Hover parks the cursor over a control so the native tooltip appears, then
#        captures the tooltip window itself to "<Out>.tooltip.png" (a tooltip is
#        a separate top-level window, so PrintWindow of our window misses it).
param(
    [string]$Exe = 'D:\Program Files (x86)\SysRecover\dist\SysRecoverUI.exe',
    [string]$Out = 'D:\ui-work\shot.png',
    [int]$WaitMs = 2500,
    [string]$Lang = '',
    [string]$Click = '',
    [string]$Hover = ''
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$sig = @'
using System;
using System.Runtime.InteropServices;
public class UiShot {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr insertAfter, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Auto)] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    public static IntPtr LParam(int x, int y) { return (IntPtr)((y << 16) | (x & 0xFFFF)); }
}
'@
if (-not ('UiShot' -as [type])) { Add-Type -TypeDefinition $sig }

# Centre of a control from skin/main.xml (panel coords == client coords).
function Get-ControlCenter([string]$name) {
    $xmlPath = Join-Path $PSScriptRoot '..\..\skin\main.xml'
    $xml = [xml](Get-Content -LiteralPath $xmlPath -Raw -Encoding UTF8)
    $node = $xml.SelectSingleNode("//*[@name='$name']")
    if (-not $node) { throw "control '$name' not found in skin/main.xml" }
    $p = $node.GetAttribute('pos').Split(',')
    $l = [int]$p[0]; $t = [int]$p[1]; $r = [int]$p[2]; $b = [int]$p[3]
    return @([int](($l + $r) / 2), [int](($t + $b) / 2))
}

# Find the tooltip window OWNED BY our process that actually has a registered
# tool and is visible. A plain FindWindow('tooltips_class32') can pick up
# Duilib's empty tooltip window (cbSize bug, see PIT-089) or another app's.
function Find-OurTooltip([IntPtr]$hMain) {
    $pidMain = 0
    [void][UiShot]::GetWindowThreadProcessId($hMain, [ref]$pidMain)
    $cur = [IntPtr]::Zero
    while ($true) {
        $cur = [UiShot]::FindWindowEx([IntPtr]::Zero, $cur, 'tooltips_class32', $null)
        if ($cur -eq [IntPtr]::Zero) { break }
        $pidT = 0
        [void][UiShot]::GetWindowThreadProcessId($cur, [ref]$pidT)
        if ($pidT -ne $pidMain) { continue }
        $tc = [UiShot]::SendMessage($cur, 1037, [IntPtr]::Zero, [IntPtr]::Zero).ToInt64()  # TTM_GETTOOLCOUNT
        if ($tc -gt 0 -and [UiShot]::IsWindowVisible($cur)) { return $cur }
    }
    return [IntPtr]::Zero
}

function Save-WindowPng([IntPtr]$h, [string]$path) {
    $r = New-Object 'UiShot+RECT'
    [void][UiShot]::GetWindowRect($h, [ref]$r)
    $w = $r.Right - $r.Left
    $ht = $r.Bottom - $r.Top
    $bmp = New-Object System.Drawing.Bitmap($w, $ht)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $blank = $true
    $hdc = $g.GetHdc()
    $pw = [UiShot]::PrintWindow($h, $hdc, 2)   # PW_RENDERFULLCONTENT
    $g.ReleaseHdc($hdc)
    if ($pw) {
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
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Output ("OK {0} {1}x{2} at {3},{4} mode={5}" -f $path, $w, $ht, $r.Left, $r.Top, $(if ($blank) { 'screen' } else { 'printwindow' }))
}

if ($Lang) { $env:SYSRECOVER_LANG = $Lang }

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

# Bring the window on top (it may be occluded by the console; SetForegroundWindow
# alone can fail due to foreground-lock rules).
[void][UiShot]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, 0x0045)   # HWND_TOPMOST + NOMOVE|NOSIZE|SHOWWINDOW
[void][UiShot]::SetForegroundWindow($h)
Start-Sleep -Milliseconds 500

if ($Click) {
    $c = Get-ControlCenter $Click
    [void][UiShot]::PostMessage($h, 0x0201, [IntPtr]1, [UiShot]::LParam($c[0], $c[1]))  # WM_LBUTTONDOWN
    [void][UiShot]::PostMessage($h, 0x0202, [IntPtr]0, [UiShot]::LParam($c[0], $c[1]))  # WM_LBUTTONUP
    Start-Sleep -Milliseconds 600
    Write-Output ("CLICK {0} at client {1},{2}" -f $Click, $c[0], $c[1])
    [void][UiShot]::SetForegroundWindow($h)
}

if ($Hover) {
    $c = Get-ControlCenter $Hover
    $org = New-Object 'UiShot+POINT'
    [void][UiShot]::ClientToScreen($h, [ref]$org)
    # Move away first: SetCursorPos to the point the cursor already occupies emits
    # no WM_MOUSEMOVE, so the hover timer never starts (and no tooltip appears).
    [void][UiShot]::SetCursorPos(5, 5)
    Start-Sleep -Milliseconds 400
    [void][UiShot]::SetCursorPos($org.X + $c[0], $org.Y + $c[1])
    Start-Sleep -Milliseconds 1600    # system hover time + tooltip show
    $tip = Find-OurTooltip $h
    if ($tip -ne [IntPtr]::Zero) {
        Save-WindowPng $tip "$Out.tooltip.png"
    } else {
        Write-Output 'WARN: no visible tooltip window (hover may not have triggered)'
    }
    [void][UiShot]::SetForegroundWindow($h)
    Start-Sleep -Milliseconds 200
}

Save-WindowPng $h $Out

Get-Process SysRecoverUI -ErrorAction SilentlyContinue | Stop-Process -Force
if ($Lang) { Remove-Item Env:\SYSRECOVER_LANG -ErrorAction SilentlyContinue }
