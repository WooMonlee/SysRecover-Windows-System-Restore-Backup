# auto-b.ps1 -- S5 guest automation for system B (ASCII only, PS5.1).
# On B's first boot the app's startup shows the 3-choice "kept old system" dialog
# (main_win.cpp:547 Ask3, buttons left->right: keep / return-old / DELETETE OLD,
# default = keep). This script clicks the RIGHTMOST button (delete old system),
# then dismisses the result MessageBox, then logs the outcome until C:\~old is
# gone. Evidence goes to <data>:\auto-b.log (host-readable via e2e-peek) and
# C:\zjgui\auto-b.log. Coordinates: Ask3 buttons are skin/fixed rects
# BtnExit(12..137) BtnMid(145..270) BtnReboot(278..403) at y 140..178 ->
# rightmost center = client (340,161). Duilib buttons are not real child windows,
# so a click is posted to the top-level dialog and hit-tested by coords.
$ErrorActionPreference = 'Continue'

$logs = @()
foreach ($d in @(Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue)) {
    try {
        if ($d.Root -and (Test-Path (Join-Path $d.Root 'images\c.esd'))) { $logs += ($d.Name + ':\auto-b.log') }
    } catch {}
}
$logs += 'C:\zjgui\auto-b.log'
function W($m) {
    $line = '[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $m
    Write-Host $line
    foreach ($lp in $logs) { try { $line | Add-Content -LiteralPath $lp -Encoding UTF8 } catch {} }
}

# --- Win32 helpers -----------------------------------------------------------
$cs = @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public class W32 {
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    public struct RECT { public int L, T, R, B; }
}
'@
$typed = $false
for ($i = 1; $i -le 3; $i++) {
    try { Add-Type -TypeDefinition $cs -ErrorAction Stop; $typed = $true; W ("Add-Type OK (attempt $i)"); break }
    catch { W ("Add-Type FAIL attempt ${i}: " + $_.Exception.Message); Start-Sleep -Seconds 3 }
}
if (-not $typed) { W 'FAIL: Add-Type unavailable'; exit 2 }

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
function ClickClient($h, [int]$x, [int]$y) {
    $lp = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
    [W32]::PostMessage($h, 0x0201, [IntPtr]1, $lp) | Out-Null   # WM_LBUTTONDOWN
    Start-Sleep -Milliseconds 60
    [W32]::PostMessage($h, 0x0202, [IntPtr]0, $lp) | Out-Null   # WM_LBUTTONUP
}
function ProbeWindows([int]$wantPid) {
    $script:pSeen = @()
    $script:pCfm = [IntPtr]::Zero
    $script:pMb = [IntPtr]::Zero
    $script:pMain = [IntPtr]::Zero
    $cb = [W32+EnumProc]{
        param($h, $l)
        if (-not [W32]::IsWindowVisible($h)) { return $true }
        $p2 = [uint32]0
        [W32]::GetWindowThreadProcessId($h, [ref]$p2) | Out-Null
        $cn = ClsName $h
        # the custom dialogs belong to our pid; the #32770 MessageBox is created by
        # the GUI thread too, so both match on the pid
        if ($p2 -ne $wantPid) { return $true }
        if ($cn -like 'tooltips*') { return $true }
        $rc = New-Object W32+RECT
        [W32]::GetWindowRect($h, [ref]$rc) | Out-Null
        $script:pSeen += ($cn + '=' + (WinTitle $h) + '@' + ($rc.R - $rc.L) + 'x' + ($rc.B - $rc.T))
        if ($cn -eq 'SysRecoverUI.Confirm' -and $script:pCfm -eq [IntPtr]::Zero) { $script:pCfm = $h }
        if ($cn -eq '#32770' -and $script:pMb -eq [IntPtr]::Zero) { $script:pMb = $h }
        if ($cn -eq 'SysRecoverUI' -and $script:pMain -eq [IntPtr]::Zero) { $script:pMain = $h }
        return $true
    }
    [W32]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
}

# --- single instance claim (the boot-time fallback may race us) --------------
$claim = 'C:\zjgui\auto-b.claimed'
try {
    New-Item -ItemType File -Path $claim -ErrorAction Stop | Out-Null
    W ('claim OK: ' + $claim + ' user=' + $env:USERNAME + ' session=' + (Get-Process -Id $PID).SessionId)
} catch {
    W ('another auto-b instance already claimed (' + $claim + ') - exiting')
    exit 0
}

# --- launch the app (the user flow: user starts the GUI, leftover dialog pops)
$exe = 'C:\Program Files\SysRecover\SysRecoverUI.exe'
if (-not (Test-Path $exe)) { W ('FAIL: exe missing: ' + $exe); exit 2 }
try { Start-Process -FilePath $exe; W ('launched gui: ' + $exe) }
catch { W ('FAIL: Start-Process: ' + $_.Exception.Message); exit 2 }

# --- wait for the GUI process ------------------------------------------------
W 'start'
W ('pre: old=' + (Test-Path 'C:\~old') + ' marker=' + (Test-Path 'C:\~old\.zj-keepold'))
$guiPid = 0
for ($i = 0; $i -lt 300; $i++) {
    Start-Sleep -Seconds 1
    $pr = @(Get-Process -Name SysRecoverUI -ErrorAction SilentlyContinue)
    if ($pr.Count -gt 0) { $guiPid = [int]$pr[0].Id; break }
    if (($i % 30) -eq 29) { W ('waiting gui process (' + ($i + 1) + 's)') }
}
if ($guiPid -eq 0) { W 'FAIL: gui process not found in 300s'; exit 2 }
W ('gui pid=' + $guiPid)

# --- answer the dialog, then poll until ~old is gone -------------------------
$asked = 0
$dismissed = 0
$t0 = Get-Date
for ($tick = 0; $tick -lt 1800; $tick++) {         # 1800 * 2s = 60 min
    Start-Sleep -Seconds 2
    $alive = @(Get-Process -Id $guiPid -ErrorAction SilentlyContinue).Count -gt 0
    ProbeWindows $guiPid
    if ($script:pCfm -ne [IntPtr]::Zero -and $asked -eq 0) {
        $mcr = New-Object W32+RECT
        [W32]::GetClientRect($script:pCfm, [ref]$mcr) | Out-Null
        W ('click DELETE (rightmost) on Confirm hwnd=' + $script:pCfm + ' title=' + (WinTitle $script:pCfm) +
           ' client=' + ($mcr.R - $mcr.L) + 'x' + ($mcr.B - $mcr.T) + ' at(340,161)')
        ClickClient $script:pCfm 340 161
        $asked = 1
    }
    if ($script:pMb -ne [IntPtr]::Zero -and $dismissed -eq 0) {
        W ('dismiss MessageBox hwnd=' + $script:pMb + ' title=' + (WinTitle $script:pMb))
        [W32]::PostMessage($script:pMb, 0x0111, [IntPtr]1, [IntPtr]::Zero) | Out-Null   # WM_COMMAND IDOK
        Start-Sleep -Milliseconds 300
        [W32]::PostMessage($script:pMb, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # WM_CLOSE fallback
        $dismissed = 1
    }
    $old = Test-Path 'C:\~old'
    if (($tick % 10) -eq 0) {
        W ('tick ' + [int]((Get-Date) - $t0).TotalSeconds + 's guiAlive=' + $alive + ' old=' + $old +
           ' asked=' + $asked + ' dismissed=' + $dismissed + ' wins=[' + ($script:pSeen -join '; ') + ']')
    }
    if ($asked -eq 1 -and $dismissed -eq 1 -and -not $old) { W 'old system gone - done'; break }
    if (-not $alive) { W 'gui process exited'; break }
}
$old2 = Test-Path 'C:\~old'
W ('post: old=' + $old2 + ' marker=' + (Test-Path 'C:\~old\.zj-keepold') + ' asked=' + $asked + ' dismissed=' + $dismissed)
if (-not $asked) { W 'FAIL: delete dialog never appeared'; exit 3 }
if ($old2) { W 'FAIL: C:\~old still present'; exit 4 }
W 'RESULT: delete-old-system OK (~old gone)'
exit 0
