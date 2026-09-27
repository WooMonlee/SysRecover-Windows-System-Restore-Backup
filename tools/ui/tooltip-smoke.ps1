# tooltip-smoke.ps1 - verify the hover tooltips actually show, and hide again.
#
# Regression guard for PIT-089: Duilib's own tooltip silently fails on this exe
# (no comctl32 v6 manifest -> v5 control rejects cbSize=sizeof(TOOLINFO)=72),
# so CMainForm manages its own tooltip (TTTOOLINFO_V1_SIZE + TTM_RELAYEVENT).
# A tooltip window existing is NOT proof it works - we assert it is visible AND
# has a registered tool (TTM_GETTOOLCOUNT > 0), then that it disappears.
#
# IMPORTANT: keep this file ASCII-only (PowerShell 5.1 parses .ps1 as ANSI when
# there is no BOM, so CJK text here breaks the parser).
#
# usage: powershell -ExecutionPolicy Bypass -File tools\ui\tooltip-smoke.ps1
#        powershell -ExecutionPolicy Bypass -File tools\ui\tooltip-smoke.ps1 -OutDir D:\ui-work
param(
    [string]$Exe = 'dist\x64\SysRecoverUI.exe',
    [string]$Root = '',
    [string]$OutDir = 'D:\ui-work'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

if (-not $Root) {
    $Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}
if (-not [System.IO.Path]::IsPathRooted($Exe)) { $Exe = Join-Path $Root $Exe }
if (-not (Test-Path -LiteralPath $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }

$sig = @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class TTip {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    public static uint PidOf(IntPtr h) { uint pid; GetWindowThreadProcessId(h, out pid); return pid; }
    public static string Rect(IntPtr h) {
        RECT r; GetWindowRect(h, out r);
        return string.Format("({0},{1})-({2},{3}) {4}x{5}", r.Left, r.Top, r.Right, r.Bottom,
            r.Right - r.Left, r.Bottom - r.Top);
    }
}
'@
if (-not ('TTip' -as [type])) { Add-Type -TypeDefinition $sig }
[void][TTip]::SetProcessDPIAware()

$xml = [xml](Get-Content -LiteralPath (Join-Path $Root 'skin\main.xml') -Raw -Encoding UTF8)
$failures = 0

function Get-ClientCenter([string]$Name) {
    $n = $xml.SelectSingleNode("//*[@name='$Name']")
    if (-not $n) { throw "control '$Name' not found in skin/main.xml" }
    $parts = $n.GetAttribute('pos') -split ','
    $l = [int]$parts[0]; $t = [int]$parts[1]; $r = [int]$parts[2]; $b = [int]$parts[3]
    return ,@((($l + $r) / 2.0), (($t + $b) / 2.0))
}

# The tooltip that counts: owned by our process, visible, and with a tool.
function Find-OurTooltip([uint32]$TargetPid) {
    $cur = [IntPtr]::Zero
    while ($true) {
        $cur = [TTip]::FindWindowEx([IntPtr]::Zero, $cur, 'tooltips_class32', $null)
        if ($cur -eq [IntPtr]::Zero) { return [IntPtr]::Zero }
        if ([TTip]::PidOf($cur) -ne $TargetPid) { continue }
        $tc = [TTip]::SendMessage($cur, 1037, [IntPtr]::Zero, [IntPtr]::Zero).ToInt64()  # TTM_GETTOOLCOUNT
        if ($tc -gt 0 -and [TTip]::IsWindowVisible($cur)) { return $cur }
    }
}

function Capture-Window([IntPtr]$H, [string]$Path) {
    $r = New-Object 'TTip+RECT'
    [void][TTip]::GetWindowRect($H, [ref]$r)
    $w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top; $pad = 12
    $bmp = New-Object System.Drawing.Bitmap(($w + 2 * $pad), ($h + 2 * $pad))
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.Left - $pad, $r.Top - $pad, 0, 0,
        (New-Object System.Drawing.Size (($w + 2 * $pad), ($h + 2 * $pad))))
    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
}

function Run-Lang([string]$Lang, [string[]]$Controls) {
    Get-Process SysRecoverUI -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 300
    $env:SYSRECOVER_LANG = $Lang
    Start-Process -FilePath $Exe | Out-Null
    Start-Sleep -Milliseconds 2500
    $h = [IntPtr]::Zero
    foreach ($q in @(Get-Process SysRecoverUI -ErrorAction SilentlyContinue)) {
        $q.Refresh(); if ($q.MainWindowHandle -ne [IntPtr]::Zero) { $h = $q.MainWindowHandle; break }
    }
    if ($h -eq [IntPtr]::Zero) { Write-Output "  ERR no window ($Lang)"; $script:failures++; return }
    $pid2 = [TTip]::PidOf($h)
    [void][TTip]::ShowWindow($h, 5)
    [void][TTip]::SetForegroundWindow($h)
    Start-Sleep -Milliseconds 500
    $wr = New-Object 'TTip+RECT'; [void][TTip]::GetWindowRect($h, [ref]$wr)
    $scale = ($wr.Right - $wr.Left) / 745.0

    foreach ($ctl in $Controls) {
        $c = Get-ClientCenter $ctl
        $sx = [int]($wr.Left + $c[0] * $scale); $sy = [int]($wr.Top + $c[1] * $scale)
        # Move away first: SetCursorPos to the point the cursor already occupies
        # emits no WM_MOUSEMOVE, so the hover timer never starts.
        [void][TTip]::SetCursorPos(20, 20); Start-Sleep -Milliseconds 400
        [void][TTip]::SetCursorPos($sx, $sy)
        Start-Sleep -Milliseconds 1600
        $tip = Find-OurTooltip $pid2
        if ($tip -eq [IntPtr]::Zero) {
            Write-Output ("  [{0}] {1}: FAIL (no visible tooltip)" -f $Lang, $ctl)
            $script:failures++
        } else {
            Capture-Window $tip (Join-Path $OutDir ("tip-{0}-{1}.png" -f $Lang, $ctl))
            Write-Output ("  [{0}] {1}: OK {2}" -f $Lang, $ctl, [TTip]::Rect($tip))
        }
        [void][TTip]::SetCursorPos([int]($wr.Left + 10 * $scale), [int]($wr.Top + 5 * $scale))
        Start-Sleep -Milliseconds 700
        $after = Find-OurTooltip $pid2
        if ($after -ne [IntPtr]::Zero) {
            Write-Output ("       hide-after-move: FAIL (still visible {0})" -f [TTip]::Rect($after))
            $script:failures++
        } else {
            Write-Output '       hide-after-move: OK'
        }
    }
    Get-Process SysRecoverUI -ErrorAction SilentlyContinue | Stop-Process -Force
    Remove-Item Env:\SYSRECOVER_LANG -ErrorAction SilentlyContinue
}

$controls = @('RepairBootBtn', 'Silent', 'BootMenuBtn')
Write-Output '=== zh ==='; Run-Lang 'zh' $controls
Write-Output '=== en ==='; Run-Lang 'en' $controls
if ($failures -gt 0) { Write-Output ("FAILED: {0} check(s)" -f $failures); exit 1 }
Write-Output 'tooltip-smoke: OK'
exit 0
