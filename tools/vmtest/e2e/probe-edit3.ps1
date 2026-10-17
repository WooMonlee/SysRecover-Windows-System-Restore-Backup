# probe-edit3.ps1 -- HOST-side fast probe, round 3 (2026-10-17)
# round 2 proved: posted WM_LBUTTONDOWN at client(440,96) creates the native
# 'EditWnd' child even when the GUI is NOT foreground, and WM_SETTEXT lands.
# round 3 isolates the "modal dialog disables the owner window" factor
# (CConfirmDlg::RunModal does EnableWindow(owner,FALSE), PIT-140): a startup
# notice dialog (rescue receipt / relocate / leftover scan) would leave the main
# window DISABLED while our posted click arrives.
# ASCII-only source (PS 5.1 parses no-BOM files as ANSI).
$ErrorActionPreference = 'Continue'
$exe = '<repo>\dist\x64\SysRecoverUI.exe'
$script:gpid = 0
$script:tops = @()

$cs = @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public class W32 {
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnableWindow(IntPtr h, bool enable);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h, uint m, IntPtr w, IntPtr l, uint flags, uint timeout, out IntPtr res);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
}
'@
try { Add-Type -TypeDefinition $cs -ErrorAction Stop } catch { Write-Host ("Add-Type FAIL: " + $_.Exception.Message); exit 2 }

function ClsName($h) { $sb = New-Object System.Text.StringBuilder 128; [W32]::GetClassName($h, $sb, 128) | Out-Null; return $sb.ToString() }
function LParam($x, $y) { [IntPtr](($y -shl 16) -bor ($x -band 65535)) }
function IsResponsive($h) { $res = [IntPtr]::Zero; $r = [W32]::SendMessageTimeout($h, 0, [IntPtr]::Zero, [IntPtr]::Zero, 3, 3000, [ref]$res); return ($r -ne [IntPtr]::Zero) }
function GetMain($pidWanted) {
    $script:gpid = $pidWanted; $script:tops = @()
    $cb = [W32+EnumProc]{ param($h, $l)
        $p = 0; [W32]::GetWindowThreadProcessId($h, [ref]$p) | Out-Null
        if ([int]$p -eq [int]$script:gpid) { if ((ClsName $h) -eq 'SysRecoverUI') { $script:tops += $h } }
        return $true }
    [W32]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    if ($script:tops.Count -gt 0) { return $script:tops[0] }
    return [IntPtr]::Zero
}
function KidClasses($main) {
    $script:kids = @()
    $cb = [W32+EnumProc]{ param($c, $l); $script:kids += (ClsName $c); return $true }
    [W32]::EnumChildWindows($main, $cb, [IntPtr]::Zero) | Out-Null
    return ($script:kids -join ',')
}
function ClickSync($h, $x, $y) {
    $lp = LParam $x $y
    $res = [IntPtr]::Zero
    $r1 = [W32]::SendMessageTimeout($h, 0x0201, [IntPtr]1, $lp, 3, 5000, [ref]$res)
    Start-Sleep -Milliseconds 100
    $r2 = [W32]::SendMessageTimeout($h, 0x0202, [IntPtr]0, $lp, 3, 5000, [ref]$res)
    return ($r1 -ne [IntPtr]::Zero)
}

Write-Host ('launching ' + $exe)
$proc = Start-Process -FilePath $exe -PassThru
Start-Sleep -Seconds 3
$gp = Get-Process -Name 'SysRecoverUI' -ErrorAction SilentlyContinue | Sort-Object StartTime | Select-Object -First 1
if (-not $gp) { Write-Host 'FAIL: GUI process not found'; exit 1 }
$main = [IntPtr]::Zero
$deadline = (Get-Date).AddSeconds(60)
while (((Get-Date) -lt $deadline) -and ($main -eq [IntPtr]::Zero)) { $main = GetMain $gp.Id; if ($main -eq [IntPtr]::Zero) { Start-Sleep -Seconds 2 } }
if ($main -eq [IntPtr]::Zero) { Write-Host 'FAIL: main window not found'; exit 1 }
$t0 = Get-Date
while (((Get-Date) -lt $t0.AddSeconds(120)) -and (-not (IsResponsive $main))) { Start-Sleep -Seconds 2 }
Start-Sleep -Seconds 4
Write-Host ('main=' + $main + ' enabled=' + [W32]::IsWindowEnabled($main) + ' kids=[' + (KidClasses $main) + ']')

# --- A: baseline click on an ENABLED window ---------------------------------
Write-Host ('A enabled-click processed=' + (ClickSync $main 440 96))
Start-Sleep -Milliseconds 1200
Write-Host ('A kids=[' + (KidClasses $main) + ']')

# --- drop focus again (TAB destroys the native EDIT, round-1 finding) -------
$res = [IntPtr]::Zero
[W32]::PostMessage($main, 0x0100, [IntPtr]0x09, [IntPtr]1) | Out-Null
Start-Sleep -Milliseconds 100
[W32]::PostMessage($main, 0x0101, [IntPtr]0x09, [IntPtr]1) | Out-Null
Start-Sleep -Milliseconds 800
Write-Host ('B after-tab kids=[' + (KidClasses $main) + ']')

# --- C: click while the window is DISABLED (modal dialog owns it) -----------
[W32]::EnableWindow($main, $false) | Out-Null
Start-Sleep -Milliseconds 300
Write-Host ('C disabled now, enabled=' + [W32]::IsWindowEnabled($main))
$c1 = ClickSync $main 440 96
Start-Sleep -Milliseconds 1200
Write-Host ('C disabled-click processed=' + $c1 + ' kids=[' + (KidClasses $main) + ']')

# --- D: re-enable and click again ------------------------------------------
[W32]::EnableWindow($main, $true) | Out-Null
Start-Sleep -Milliseconds 300
$d1 = ClickSync $main 440 96
Start-Sleep -Milliseconds 1200
Write-Host ('D reenabled-click processed=' + $d1 + ' kids=[' + (KidClasses $main) + ']')

Write-Host 'closing GUI'
[W32]::PostMessage($main, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Start-Sleep -Seconds 2
$still = @(Get-Process -Name 'SysRecoverUI' -ErrorAction SilentlyContinue)
if ($still.Count -gt 0) { $still | Stop-Process -Force -ErrorAction SilentlyContinue; Write-Host 'GUI force-killed' } else { Write-Host 'GUI exited cleanly' }
Write-Host 'probe done'
