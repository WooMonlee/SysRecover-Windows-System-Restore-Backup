# auto-a.ps1 -- unattended GUI restore driver (system A -> system B), v2 2026-10-17
# Flow: launch SysRecoverUI -> wait main window + readiness -> WM_SETTEXT image
#       path into the native EDIT -> click MainAction -> answer the Ask3 confirm
#       with the DEFAULT button ("restore then reboot" = swap + keepOld) ->
#       wait for staging + reboot (QEMU exits under -no-reboot).
#
# v3 fixes (S3 leg2' run: the GUI came up fine but we logged
#   "FAIL: main window not found in 300s" although the process kept reporting
#   MainWindowHandle=262486; GPO autologon hook itself was proven good):
#   * main-window discovery is now PROCESS-ID based (EnumWindows + window owner
#     pid; prefer class 'SysRecoverUI', else the first visible, titled,
#     non-tooltip top-level window). Class-name FindWindow() returned 0 in-guest
#     for reasons unknown; every candidate is now logged as class=title;
#   * the confirm dialog is found the same way (visible top-level window of the
#     GUI pid that is neither the main window nor a tooltip);
#   * the ImagePath box is CLICKED first (client ~440,96): Duilib only creates
#     the native child EDIT on UIEVENT_SETFOCUS (UIEdit.cpp:299-306), so v2's
#     "just WM_SETTEXT whatever EDIT child exists" found nothing;
#   * that child's class is 'EditWnd' (UIEdit.cpp:113 GetWindowClassName +
#     WC_EDIT superclass), NOT 'Edit' -- v2's equality test could never hit;
#   * never post VK_ESCAPE: WindowImplBase closes the main window on it
#     (WinImplBase.cpp:68). The pick-menu on EN_SETFOCUS needs non-empty
#     m_searchHits (main_form.cpp:315) and we never search -> no popup at all.
#
# v2 fixes (S3 leg2 run had "FAIL: main window not found in 90s"):
#   * Add-Type is retried 3x and its failure is LOGGED (v1 swallowed it silently);
#   * window lookup cross-checks Get-Process MainWindowHandle (no P/Invoke needed);
#   * the partition combo is auto-selected by the GUI after enumeration
#     (main_form.cpp:1167 m_selPart=0), so the real-cursor combo interaction is
#     GONE -- everything is PostMessage, no screen-position dependency;
#   * the GUI startup blocks the UI thread ~90s in QEMU (diag+collect), so we wait
#     for responsiveness (SendMessageTimeout) and for 'cleanup-stray' in the GUI
#     log before touching anything;
#   * the confirm is answered with posted VK_RETURN (defaultIdx=1 = BtnMid,
#     confirm_dlg.cpp:123 Finish(m_defaultIdx)) + coordinate-click fallback;
#   * the GUI's own log tail is mirrored into our log (single-file forensics).
# ASCII-only source (PS 5.1 parses no-BOM files as ANSI).
$ErrorActionPreference = 'Continue'
$gui = 'C:\zjgui\SysRecoverUI.exe'
$guiLogDir = 'D:\ZJRESTORE\logs'

# --- detect the data drive holding the test images + dual log destinations ----
$dataDrive = $null
$logs = @()
foreach ($d in (Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue)) {
    try {
        if (Test-Path (Join-Path $d.Root 'images\c.esd')) {
            if (-not $dataDrive) { $dataDrive = $d.Name }
            $logs += ($d.Name + ':\auto-a.log')
        }
    } catch {}
}
$logs += 'C:\zjgui\auto-a.log'
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
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextLength(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, string l);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h, uint m, IntPtr w, IntPtr l, uint flags, uint timeout, out IntPtr res);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    // v4 additions: foreground/focus/capture forensics + window activation.
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vk);
    [DllImport("user32.dll")] public static extern bool GetGUIThreadInfo(uint tid, ref GUITHREADINFO gti);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] public struct GUITHREADINFO {
        public int cbSize; public uint flags;
        public IntPtr hwndActive, hwndFocus, hwndCapture, hwndMenuOwner, hwndMoveSize, hwndCaret;
        public RECT rcCaret;
    }
}
'@
$typed = $false
for ($i = 1; $i -le 3; $i++) {
    try { Add-Type -TypeDefinition $cs -ErrorAction Stop; $typed = $true; W ("Add-Type OK (attempt $i)"); break }
    catch { W ("Add-Type FAIL attempt ${i}: " + $_.Exception.Message); Start-Sleep -Seconds 3 }
}
if (-not $typed) { W 'FAIL: Add-Type unavailable -> cannot automate'; exit 2 }

function LParam($x, $y) { [IntPtr](($y -shl 16) -bor ($x -band 65535)) }
# v4: send the click SYNCHRONOUSLY (SendMessageTimeout) so the return value tells
# us whether the GUI thread actually processed it (0 = hung/timed out), then also
# Post the same pair as a belt-and-braces fallback. A WM_MOUSEMOVE is sent first
# (real clicks always have one; Duilib tracks hover/mouse position).
function ClickClient($h, $x, $y) {
    $lp = LParam $x $y
    $res = [IntPtr]::Zero
    $ok1 = ([W32]::SendMessageTimeout($h, 0x0200, [IntPtr]0, $lp, 3, 5000, [ref]$res)) -ne [IntPtr]::Zero
    Start-Sleep -Milliseconds 40
    $ok2 = ([W32]::SendMessageTimeout($h, 0x0201, [IntPtr]1, $lp, 3, 5000, [ref]$res)) -ne [IntPtr]::Zero
    Start-Sleep -Milliseconds 120
    $ok3 = ([W32]::SendMessageTimeout($h, 0x0202, [IntPtr]0, $lp, 3, 5000, [ref]$res)) -ne [IntPtr]::Zero
    [W32]::PostMessage($h, 0x0201, [IntPtr]1, $lp) | Out-Null
    Start-Sleep -Milliseconds 70
    [W32]::PostMessage($h, 0x0202, [IntPtr]0, $lp) | Out-Null
    Start-Sleep -Milliseconds 250
    return ($ok2 -and $ok3)
}
# who owns the foreground / focus / mouse capture, for the GUI's UI thread
function GuiFocusInfo($h) {
    $fp = [uint32]0
    $tid = [W32]::GetWindowThreadProcessId($h, [ref]$fp)
    $gti = New-Object W32+GUITHREADINFO
    $gti.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf($gti)
    if (-not [W32]::GetGUIThreadInfo($tid, [ref]$gti)) { return 'gti=fail' }
    return ('fg=' + [W32]::GetForegroundWindow().ToString() +
            ' focus=' + $gti.hwndFocus.ToString() +
            ' capture=' + $gti.hwndCapture.ToString() +
            ' active=' + $gti.hwndActive.ToString())
}
# bring the GUI to the foreground (best effort; posted messages work either way)
function MakeForeground($h) {
    [W32]::ShowWindow($h, 5) | Out-Null      # SW_SHOW
    [W32]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, 0x0043) | Out-Null  # TOPMOST|NOMOVE|NOSIZE|SHOWWIN
    [W32]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 400
}
function PostKey($h, $vk) {
    [W32]::PostMessage($h, 0x0100, [IntPtr]$vk, [IntPtr]1) | Out-Null   # WM_KEYDOWN
    Start-Sleep -Milliseconds 60
    [W32]::PostMessage($h, 0x0101, [IntPtr]$vk, [IntPtr]1) | Out-Null   # WM_KEYUP
}
function IsResponsive($h) {
    $res = [IntPtr]::Zero
    $r = [W32]::SendMessageTimeout($h, 0, [IntPtr]::Zero, [IntPtr]::Zero, 3, 3000, [ref]$res)
    return ($r -ne [IntPtr]::Zero)
}
function WinTitle($h) {
    $sb = New-Object System.Text.StringBuilder 256
    [W32]::GetWindowText($h, $sb, 256) | Out-Null
    return $sb.ToString()
}
function ClsName($h) {
    $sb = New-Object System.Text.StringBuilder 128
    [W32]::GetClassName($h, $sb, 128) | Out-Null
    return $sb.ToString()
}
# enumerate EDIT children; the ImagePath one sits at client y 77..115 -> match on
# screen-rect converted into main-window client space (child GetClientRect is
# always (0,0,w,h), so v1's y-band test could never match - fixed here).
# v4: every failure inside the callback is captured ($script:editErr) instead of
#     silently aborting EnumChildWindows, and $script:editCb counts invocations so
#     "no children" can be told apart from "callback never ran".
function Get-ImagePathEdit($main) {
    $script:editHit = [IntPtr]::Zero
    $script:editSeen = @()
    $script:editCls = @()
    $script:editCand = @()
    $script:editCb = 0
    $script:editErr = ''
    $cb = [W32+EnumProc]{
        param($h, $l)
        $script:editCb = $script:editCb + 1
        try {
            $sb = New-Object System.Text.StringBuilder 128
            [W32]::GetClassName($h, $sb, 128) | Out-Null
            $cn = $sb.ToString()
            $script:editCls += $cn
            if ($cn -like 'Edit*') {
                $rc = New-Object W32+RECT
                [W32]::GetWindowRect($h, [ref]$rc) | Out-Null
                $p = New-Object W32+POINT
                $p.X = $rc.L; $p.Y = $rc.T
                [W32]::ScreenToClient($script:mainH, [ref]$p) | Out-Null
                $script:editSeen += ('edit@' + $p.X + ',' + $p.Y)
                if ($p.Y -ge 60 -and $p.Y -le 140) { $script:editCand += ,@($p.Y, $p.X, $h) }
            }
        } catch { if ($script:editErr -eq '') { $script:editErr = $_.Exception.Message } }
        return $true
    }
    [W32]::EnumChildWindows($main, $cb, [IntPtr]::Zero) | Out-Null
    # pick the candidate whose top edge is closest to the known image-box value
    # (host probe: child rect 261,92,622,104 -> top y=92) so the note-box EDIT
    # further down (y~135) can never win the y-band match.
    $best = 9999
    foreach ($c in $script:editCand) {
        $d = [int]$c[0] - 98
        if ($d -lt 0) { $d = -$d }
        if ($d -lt $best) { $best = $d; $script:editHit = $c[2] }
    }
    return $script:editHit
}
function FindConfirm {
    $script:cfmH = [IntPtr]::Zero
    $cb = [W32+EnumProc]{
        param($h, $l)
        if (-not [W32]::IsWindowVisible($h)) { return $true }
        $p2 = [uint32]0
        [W32]::GetWindowThreadProcessId($h, [ref]$p2) | Out-Null
        if ($p2 -ne $script:guiPid) { return $true }
        $cn = ClsName $h
        if ($cn -like 'tooltips*' -or $cn -eq 'SysRecoverUI') { return $true }
        $script:cfmH = $h
        return $false
    }
    [W32]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    return $script:cfmH
}
function FindErrDlg($wantPid) {
    $script:errH = [IntPtr]::Zero
    $script:dialogSeen = @()
    $cb = [W32+EnumProc]{
        param($h, $l)
        $sb = New-Object System.Text.StringBuilder 64
        [W32]::GetClassName($h, $sb, 64) | Out-Null
        $cn = $sb.ToString()
        if ($cn -eq '#32770' -or $cn -like 'SysRecoverUI*') {
            $p2 = [uint32]0
            [W32]::GetWindowThreadProcessId($h, [ref]$p2) | Out-Null
            if ($p2 -eq $script:guiPid) {
                $script:dialogSeen += ($cn + '=' + (WinTitle $h))
                if ($cn -eq '#32770' -and -not $script:errH) { $script:errH = $h }
            }
        }
        return $true
    }
    [W32]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    return $script:errH
}
# close stray console windows (cmd/powershell) so they cannot cover the GUI; never
# our own console (pid filter)
function CloseStrayConsoles {
    $cb = [W32+EnumProc]{
        param($h, $l)
        $sb = New-Object System.Text.StringBuilder 64
        [W32]::GetClassName($h, $sb, 64) | Out-Null
        if ($sb.ToString() -eq 'ConsoleWindowClass') {
            $p2 = [uint32]0
            [W32]::GetWindowThreadProcessId($h, [ref]$p2) | Out-Null
            if ($p2 -ne $script:myPid) {
                [W32]::PostMessage($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # WM_CLOSE
                $script:consolesClosed++
            }
        }
        return $true
    }
    $script:consolesClosed = 0
    [W32]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    return $script:consolesClosed
}
function GuiLines {
    try {
        $f = Get-ChildItem -LiteralPath $guiLogDir -Filter 'SysRecover-*.log' -ErrorAction SilentlyContinue |
             Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if (-not $f) { return @() }
        return @(Get-Content -LiteralPath $f.FullName -ErrorAction SilentlyContinue)
    } catch { return @() }
}
function GuiTail($n) {
    $all = GuiLines
    if ($all.Count -eq 0) { return @() }
    return @($all | Select-Object -Last $n)
}
# only the lines written by THIS boot: older boots append to the same daily file,
# so markers like 'image loaded' / 'cleanup-stray' could otherwise match stale text
function GuiNew {
    $all = GuiLines
    if ($script:guiMark -ge $all.Count) { return @() }
    return @($all | Select-Object -Skip $script:guiMark)
}
function WGui($tag) { foreach ($l in (GuiTail 3)) { W ("gui|$tag| " + $l) } }

$script:mainH = [IntPtr]::Zero
$script:myPid = $PID
$milestones = @{
    'restore path: staged-reboot' = 'staging path = staged-reboot';
    'A-engine: extract to'        = 'A-engine extract started';
    'A-engine: ~new ready'        = 'A-engine ~new ready';
    'keepold marker written'      = 'keepold marker written';
    'restore staged'              = 'restore staged';
    'pending'                     = 'pending-restore armed';
}
$milestoneSeen = @{}
#    otherwise the first visible titled non-tooltip window is used (logged).
function Get-GuiWindows {
    $script:pids = @()
    foreach ($pr in @(Get-Process -Name SysRecoverUI -ErrorAction SilentlyContinue)) {
        $script:pids += [uint32]$pr.Id
    }
    $script:winSeen = @()
    $script:cands = @()
    $script:winBest = [IntPtr]::Zero
    if ($script:pids.Count -eq 0) { return }
    $cb = [W32+EnumProc]{
        param($h, $l)
        $p2 = [uint32]0
        [W32]::GetWindowThreadProcessId($h, [ref]$p2) | Out-Null
        if ($script:pids -notcontains $p2) { return $true }
        $cn = ClsName $h
        $vis = [W32]::IsWindowVisible($h)
        $ttl = WinTitle $h
        $rc = New-Object W32+RECT
        [W32]::GetWindowRect($h, [ref]$rc) | Out-Null
        $area = ($rc.R - $rc.L) * ($rc.B - $rc.T)
        if ($vis) { $script:winSeen += ($cn + '=' + $ttl + '@' + $rc.L + ',' + $rc.T + ',' + ($rc.R - $rc.L) + 'x' + ($rc.B - $rc.T)) }
        if (-not $vis) { return $true }
        if ($cn -eq 'SysRecoverUI') { $script:cands += ,@($area, $h); return $true }
        if ($script:winBest -eq [IntPtr]::Zero -and $cn -notlike 'tooltips*' -and $ttl -ne '') {
            $script:winBest = $h
        }
        return $true
    }
    [W32]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    # v4: prefer the LARGEST visible 'SysRecoverUI' window -- a 0x0 / ghost window
    # of the same class would otherwise win the enumeration and swallow every
    # posted click (no control at that point -> no native EDIT, no error).
    $bestArea = -1
    foreach ($c in $script:cands) {
        if ([int]$c[0] -gt $bestArea) { $bestArea = [int]$c[0]; $script:winBest = $c[1] }
    }
}
# v4: close any visible top-level window of the GUI pid other than the main window
# (startup notice dialogs -- rescue receipt / relocate / leftover scan -- disable
# the main window while up). WM_CLOSE is the dismiss/acknowledge path of our
# Ask2/Ask3 dialogs and never triggers their destructive default action.
function CloseGuiDialogs($main) {
    $script:dlgSeen = @()
    $script:dlgClosed = 0
    $cb = [W32+EnumProc]{
        param($h, $l)
        if ($h -eq $script:mainH) { return $true }
        $p2 = [uint32]0
        [W32]::GetWindowThreadProcessId($h, [ref]$p2) | Out-Null
        if ($p2 -ne $script:guiPid) { return $true }
        if (-not [W32]::IsWindowVisible($h)) { return $true }
        $cn = ClsName $h
        if ($cn -like 'tooltips*') { return $true }
        $rc = New-Object W32+RECT
        [W32]::GetWindowRect($h, [ref]$rc) | Out-Null
        if ((($rc.R - $rc.L) * ($rc.B - $rc.T)) -lt 4000) { return $true }
        $script:dlgSeen += ($cn + '=' + (WinTitle $h))
        [W32]::PostMessage($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        $script:dlgClosed = $script:dlgClosed + 1
        return $true
    }
    [W32]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
}
# ---- v4 driver: exercise the exact helper library above against the local GUI ----
$exe = '<repo>\dist\x64\SysRecoverUI.exe'
Write-Host ('launching ' + $exe)
$proc = Start-Process -FilePath $exe -PassThru
Start-Sleep -Seconds 3
$main = [IntPtr]::Zero
for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Seconds 1
    Get-GuiWindows
    $main = $script:winBest
    if ($main -ne [IntPtr]::Zero) { break }
}
if ($main -eq [IntPtr]::Zero) { Write-Host 'FAIL: no main window'; exit 1 }
$script:mainH = $main
$wp = [uint32]0
[W32]::GetWindowThreadProcessId($main, [ref]$wp) | Out-Null
$script:guiPid = $wp
$mrc = New-Object W32+RECT
[W32]::GetWindowRect($main, [ref]$mrc) | Out-Null
$mcr = New-Object W32+RECT
[W32]::GetClientRect($main, [ref]$mcr) | Out-Null
Write-Host ('main=' + $main + ' pid=' + $wp + ' cls=' + (ClsName $main) + ' title=' + (WinTitle $main) + ' rect=' + $mrc.L + ',' + $mrc.T + ',' + ($mrc.R - $mrc.L) + 'x' + ($mrc.B - $mrc.T) + ' client=' + ($mcr.R - $mcr.L) + 'x' + ($mcr.B - $mcr.T) + ' enabled=' + [W32]::IsWindowEnabled($main))
Write-Host ('pid windows: [' + ($script:winSeen -join '; ') + ']')
CloseGuiDialogs $main
Write-Host ('dialogs closed=' + $script:dlgClosed + ' seen=[' + ($script:dlgSeen -join '|') + ']')
$t0 = Get-Date
while (((Get-Date) -lt $t0.AddSeconds(120)) -and (-not (IsResponsive $main))) { Start-Sleep -Seconds 2 }
Start-Sleep -Seconds 6
Write-Host ('focus info before: ' + (GuiFocusInfo $main))
MakeForeground $main
$edit = [IntPtr]::Zero
$pts = @(@(440, 96), @(350, 96), @(520, 96), @(440, 86), @(440, 106))
for ($p = 0; $p -lt $pts.Count; $p++) {
    $x = [int]$pts[$p][0]; $y = [int]$pts[$p][1]
    $proc2 = ClickClient $main $x $y
    Write-Host ('click ' + ($p + 1) + '/' + $pts.Count + ' client ' + $x + ',' + $y + ' processed=' + $proc2 + ' | ' + (GuiFocusInfo $main))
    Start-Sleep -Milliseconds 1200
    $edit = Get-ImagePathEdit $main
    Write-Host ('  try ' + ($p + 1) + ': cb=' + $script:editCb + ' raw=[' + ($script:editCls -join ' ') + '] edits=[' + ($script:editSeen -join ' ') + '] err=' + $script:editErr)
    if ($edit -ne [IntPtr]::Zero) { break }
}
if ($edit -eq [IntPtr]::Zero) {
    Write-Host 'click route failed -> tab route'
    for ($p = 1; $p -le 10; $p++) {
        PostKey $main 0x09
        Start-Sleep -Milliseconds 600
        $edit = Get-ImagePathEdit $main
        Write-Host ('  tab ' + $p + ': cb=' + $script:editCb + ' raw=[' + ($script:editCls -join ' ') + '] edits=[' + ($script:editSeen -join ' ') + ']')
        if ($edit -ne [IntPtr]::Zero) { break }
    }
}
if ($edit -eq [IntPtr]::Zero) {
    Write-Host 'FAIL: ImagePath EDIT not found'
} else {
    Write-Host ('image EDIT hwnd=' + $edit + ' cls=' + (ClsName $edit))
    [W32]::SendMessage($edit, 0x000C, [IntPtr]::Zero, 'D:\images\c.esd') | Out-Null
    Start-Sleep -Milliseconds 500
    $back = New-Object System.Text.StringBuilder 512
    [W32]::GetWindowText($edit, $back, 512) | Out-Null
    Write-Host ('WM_SETTEXT readback=' + $back.ToString())
}
Write-Host 'closing GUI'
[W32]::PostMessage($main, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Start-Sleep -Seconds 2
$still = @(Get-Process -Name 'SysRecoverUI' -ErrorAction SilentlyContinue)
if ($still.Count -gt 0) { $still | Stop-Process -Force -ErrorAction SilentlyContinue; Write-Host 'GUI force-killed' } else { Write-Host 'GUI exited cleanly' }
Write-Host 'probe4 done'
