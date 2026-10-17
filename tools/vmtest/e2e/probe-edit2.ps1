# probe-edit2.ps1 -- HOST-side fast probe, round 2 (2026-10-17)
# round 1 proved: posted WM_LBUTTONDOWN at client(440,96) DOES create the native
# child EDIT (cls=EditWnd) and VK_TAB destroys it again.
# round 2 questions:
#   (a) does it still work when the GUI is NOT the foreground window
#       (guest: desktop/console was in front) -> -StealFocus
#   (b) does WM_SETTEXT land, does WM_GETTEXT read it back, and does the GUI log
#       then show 'GUI image loaded' (m_wimPath set = real goal) ?
# ASCII-only source (PS 5.1 parses no-BOM files as ANSI).
param([switch]$StealFocus)
$ErrorActionPreference = 'Continue'
$exe = '<repo>\dist\x64\SysRecoverUI.exe'
$script:gpid = 0
$script:tops = @()

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
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, string l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, StringBuilder l);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h, uint m, IntPtr w, IntPtr l, uint flags, uint timeout, out IntPtr res);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
'@
try { Add-Type -TypeDefinition $cs -ErrorAction Stop } catch { Write-Host ("Add-Type FAIL: " + $_.Exception.Message); exit 2 }

function ClsName($h) { $sb = New-Object System.Text.StringBuilder 128; [W32]::GetClassName($h, $sb, 128) | Out-Null; return $sb.ToString() }
function WinTitle($h) { $sb = New-Object System.Text.StringBuilder 256; [W32]::GetWindowText($h, $sb, 256) | Out-Null; return $sb.ToString() }
function LParam($x, $y) { [IntPtr](($y -shl 16) -bor ($x -band 65535)) }
function IsResponsive($h) { $res = [IntPtr]::Zero; $r = [W32]::SendMessageTimeout($h, 0, [IntPtr]::Zero, [IntPtr]::Zero, 3, 3000, [ref]$res); return ($r -ne [IntPtr]::Zero) }

function GetMain($pidWanted) {
    $script:gpid = $pidWanted
    $script:tops = @()
    $cb = [W32+EnumProc]{ param($h, $l)
        $p = 0; [W32]::GetWindowThreadProcessId($h, [ref]$p) | Out-Null
        if ([int]$p -eq [int]$script:gpid) { if ((ClsName $h) -eq 'SysRecoverUI') { $script:tops += $h } }
        return $true }
    [W32]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    if ($script:tops.Count -gt 0) { return $script:tops[0] }
    return [IntPtr]::Zero
}
function ListKids($main) {
    $script:kids = @()
    $cb = [W32+EnumProc]{ param($c, $l)
        $rc = New-Object W32+RECT; [W32]::GetWindowRect($c, [ref]$rc) | Out-Null
        $p1 = New-Object W32+POINT; $p1.X = $rc.L; $p1.Y = $rc.T
        [W32]::ScreenToClient($script:mainH, [ref]$p1) | Out-Null
        $p2 = New-Object W32+POINT; $p2.X = $rc.R; $p2.Y = $rc.B
        [W32]::ScreenToClient($script:mainH, [ref]$p2) | Out-Null
        $script:kids += ('h=' + $c + ' cls=' + (ClsName $c) + ' client=' + $p1.X + ',' + $p1.Y + ',' + $p2.X + ',' + $p2.Y + ' text=[' + (WinTitle $c) + ']')
        return $true }
    [W32]::EnumChildWindows($main, $cb, [IntPtr]::Zero) | Out-Null
    return $script:kids
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
$script:mainH = $main
$mr = New-Object W32+RECT; [W32]::GetWindowRect($main, [ref]$mr) | Out-Null
Write-Host ('main hwnd=' + $main + ' pid=' + $gp.Id + ' rect=' + $mr.L + ',' + $mr.T + ',' + $mr.R + ',' + $mr.B)

$t0 = Get-Date
while (((Get-Date) -lt $t0.AddSeconds(120)) -and (-not (IsResponsive $main))) { Start-Sleep -Seconds 2 }
Write-Host ('responsive after ' + [int]((Get-Date) - $t0).TotalSeconds + 's')
Start-Sleep -Seconds 4

$np = $null
if ($StealFocus) {
    Write-Host 'stealing focus with notepad'
    $np = Start-Process notepad -PassThru
    Start-Sleep -Seconds 3
}
$fg = [W32]::GetForegroundWindow()
Write-Host ('foreground=' + $fg + ' (main=' + $main + ') isMainForeground=' + ($fg -eq $main))
Write-Host ('kids before: ' + ((ListKids $main) -join ' | '))

$lp = LParam 440 96
[W32]::PostMessage($main, 0x0201, [IntPtr]1, $lp) | Out-Null
Start-Sleep -Milliseconds 120
[W32]::PostMessage($main, 0x0202, [IntPtr]0, $lp) | Out-Null
Start-Sleep -Milliseconds 1500
$kids = ListKids $main
Write-Host ('kids after click: ' + ($kids -join ' | '))

# find the SkinEdit child (cls EditWnd) and push the path in
$edit = [IntPtr]::Zero
foreach ($k in $kids) { if ($k -match 'cls=EditWnd') { $edit = [IntPtr][int64](($k -split ' ')[0] -replace 'h=', ''); break } }
if ($edit -eq [IntPtr]::Zero) { Write-Host 'VERDICT: no EditWnd created'; }
else {
    Write-Host ('EditWnd=' + $edit)
    $path = 'D:\images\c.esd'
    [W32]::SendMessage($edit, 0x000C, [IntPtr]::Zero, $path) | Out-Null   # WM_SETTEXT
    Start-Sleep -Milliseconds 1500
    $sb = New-Object System.Text.StringBuilder 512
    [W32]::SendMessage($edit, 0x000D, [IntPtr]512, $sb) | Out-Null       # WM_GETTEXT
    Write-Host ('WM_GETTEXT readback=[' + $sb.ToString() + ']')
    Start-Sleep -Seconds 3
}

# GUI log tails (candidate log roots)
$roots = @('D:\ZJRESTORE\logs', '<repo>\dist\logs', '<repo>\logs')
foreach ($r in $roots) {
    $f = Get-ChildItem -LiteralPath $r -Filter 'SysRecover-*.log' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($f) { Write-Host ('--- log ' + $f.FullName + ' (last 20) ---'); Get-Content -LiteralPath $f.FullName -Tail 20 | ForEach-Object { Write-Host ('  ' + $_) } }
}

Write-Host 'closing GUI'
[W32]::PostMessage($main, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Start-Sleep -Seconds 2
if ($np) { Stop-Process -Id $np.Id -Force -ErrorAction SilentlyContinue }
$still = @(Get-Process -Name 'SysRecoverUI' -ErrorAction SilentlyContinue)
if ($still.Count -gt 0) { $still | Stop-Process -Force -ErrorAction SilentlyContinue; Write-Host 'GUI force-killed' } else { Write-Host 'GUI exited cleanly' }
Write-Host 'probe done'
