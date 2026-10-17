# probe-edit.ps1 -- HOST-side fast probe (2026-10-17)
# Question: after PostMessage(WM_LBUTTONDOWN) at client (440,96) on the main
# window, does Duilib create the native child EDIT ('EditWnd')?  If not, does
# keyboard focus (VK_TAB) create it?  Runs the real GUI locally: no QEMU,
# no injection, seconds per iteration.
# ASCII-only source (PS 5.1 parses no-BOM files as ANSI).
$ErrorActionPreference = 'Continue'
$exe = '<repo>\dist\x64\SysRecoverUI.exe'
$script:gpid = 0
$script:cbCount = 0

$cs = @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public class W32 {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h, uint m, IntPtr w, IntPtr l, uint flags, uint timeout, out IntPtr res);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern IntPtr GetFocus();
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
'@
try { Add-Type -TypeDefinition $cs -ErrorAction Stop } catch { Write-Host ("Add-Type FAIL: " + $_.Exception.Message); exit 2 }

function ClsName($h) {
    $sb = New-Object System.Text.StringBuilder 128
    [W32]::GetClassName($h, $sb, 128) | Out-Null
    return $sb.ToString()
}
function WinTitle($h) {
    $sb = New-Object System.Text.StringBuilder 256
    [W32]::GetWindowText($h, $sb, 256) | Out-Null
    return $sb.ToString()
}
function LParam($x, $y) { [IntPtr](($y -shl 16) -bor ($x -band 65535)) }
function IsResponsive($h) {
    $res = [IntPtr]::Zero
    $r = [W32]::SendMessageTimeout($h, 0, [IntPtr]::Zero, [IntPtr]::Zero, 3, 3000, [ref]$res)
    return ($r -ne [IntPtr]::Zero)
}

# collect everything first, print AFTER EnumWindows returns (no PS reentry)
$script:tops = @()
function Dump($tag) {
    $script:tops = @()
    $script:cbCount = 0
    $cb = [W32+EnumProc]{
        param($h, $l)
        $p = 0
        [W32]::GetWindowThreadProcessId($h, [ref]$p) | Out-Null
        if ([int]$p -eq [int]$script:gpid) {
            $script:tops += ('TOP h=' + $h + ' cls=' + (ClsName $h) + ' vis=' + [W32]::IsWindowVisible($h) + ' title=' + (WinTitle $h))
        }
        return $true
    }
    [W32]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    Write-Host ("--- dump " + $tag + " ---")
    foreach ($t in $script:tops) { Write-Host $t }
    # children of every top-level window of the pid
    foreach ($t in $script:tops) {
        $hh = [IntPtr][int64]((($t -split ' ')[1]) -replace 'h=', '')
        $script:kids = @()
        $cb2 = [W32+EnumProc]{
            param($c, $l2)
            $script:cbCount++
            $rc = New-Object W32+RECT
            [W32]::GetWindowRect($c, [ref]$rc) | Out-Null
            $script:kids += ('   CH h=' + $c + ' cls=' + (ClsName $c) + ' rect=' + $rc.L + ',' + $rc.T + ',' + $rc.R + ',' + $rc.B)
            return $true
        }
        [W32]::EnumChildWindows($hh, $cb2, [IntPtr]::Zero) | Out-Null
        Write-Host ('  [' + $hh + '] children=' + $script:kids.Count + ' cbCalled=' + $script:cbCount)
        foreach ($k in $script:kids) { Write-Host $k }
    }
}

# --- launch ------------------------------------------------------------------
Write-Host ('launching ' + $exe)
$proc = Start-Process -FilePath $exe -PassThru
Start-Sleep -Seconds 3
$guiProcs = @(Get-Process -Name 'SysRecoverUI' -ErrorAction SilentlyContinue)
Write-Host ('procs: ' + (($guiProcs | ForEach-Object { $_.Id.ToString() + ':' + $_.MainWindowHandle }) -join ' '))

# wait for the main window (pid-based, same as auto-a v3)
$main = [IntPtr]::Zero
$deadline = (Get-Date).AddSeconds(60)
while ((Get-Date) -lt $deadline) {
    $found = $null
    foreach ($gp in $guiProcs) {
        $script:gpid = $gp.Id
        $script:tops = @()
        $cb = [W32+EnumProc]{
            param($h, $l)
            $p = 0
            [W32]::GetWindowThreadProcessId($h, [ref]$p) | Out-Null
            if ([int]$p -eq [int]$script:gpid) {
                if ((ClsName $h) -eq 'SysRecoverUI') { $script:tops += $h }
            }
            return $true
        }
        [W32]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
        if ($script:tops.Count -gt 0) { $found = $script:tops[0]; break }
    }
    if ($found) { $main = $found; break }
    Start-Sleep -Seconds 2
}
if ($main -eq [IntPtr]::Zero) { Write-Host 'FAIL: main window not found'; exit 1 }
$script:gpid = (Get-Process -Id (Get-Process -Name 'SysRecoverUI' | Select-Object -First 1).Id).Id
foreach ($gp in (Get-Process -Name 'SysRecoverUI' -ErrorAction SilentlyContinue)) {
    $script:gpid = $gp.Id
}
Write-Host ('main hwnd=' + $main + ' pid=' + $script:gpid + ' cls=' + (ClsName $main) + ' title=' + (WinTitle $main))

# wait for responsiveness + let the startup block (~diag+collect+cleanup) finish
$t0 = Get-Date
while (((Get-Date) -lt $t0.AddSeconds(120)) -and (-not (IsResponsive $main))) { Start-Sleep -Seconds 2 }
Write-Host ('responsive after ' + [int]((Get-Date) - $t0).TotalSeconds + 's')
Start-Sleep -Seconds 5
Dump 'before-click'

# --- path 1: posted mouse click on the image box (client 440,96) -------------
$lp = LParam 440 96
[W32]::PostMessage($main, 0x0201, [IntPtr]1, $lp) | Out-Null
Start-Sleep -Milliseconds 120
[W32]::PostMessage($main, 0x0202, [IntPtr]0, $lp) | Out-Null
Start-Sleep -Milliseconds 1200
Dump 'after-click'

# --- path 2: keyboard focus via VK_TAB --------------------------------------
for ($i = 1; $i -le 8; $i++) {
    [W32]::PostMessage($main, 0x0100, [IntPtr]0x09, [IntPtr]1) | Out-Null
    Start-Sleep -Milliseconds 80
    [W32]::PostMessage($main, 0x0101, [IntPtr]0x09, [IntPtr]1) | Out-Null
    Start-Sleep -Milliseconds 300
    Dump ('after-tab-' + $i)
}

# --- cleanup -----------------------------------------------------------------
Write-Host 'closing GUI'
[W32]::PostMessage($main, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # WM_CLOSE
Start-Sleep -Seconds 2
$still = @(Get-Process -Name 'SysRecoverUI' -ErrorAction SilentlyContinue)
if ($still.Count -gt 0) {
    Write-Host ('GUI still alive -> Stop-Process (' + $still.Count + ')')
    $still | Stop-Process -Force -ErrorAction SilentlyContinue
} else {
    Write-Host 'GUI exited cleanly'
}
Write-Host 'probe done'
