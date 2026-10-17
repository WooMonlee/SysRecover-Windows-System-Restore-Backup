# probe-drop.ps1 -- host-side check of the v5 image-feed route (WM_DROPFILES).
#
# Why: auto-a.ps1 v4 fed the image path by focusing the box and WM_SETTEXT-ing the
# native child EDIT. That sets m_wimPath (main_form.cpp:343 EN_CHANGE handler) but
# leaves the Duilib CONTROL text empty -- that handler returns 0, so the
# notification never reaches the child's OnEditChanged (UIEdit.cpp:145). As soon as
# the child dies on focus loss, the 500ms path-sync timer (main_form.cpp:187 ->
# :380 SyncImagePathFromUi, which reads the control text when there is no child)
# overwrites m_wimPath with "" -> StartRestoreAsync (main_form.cpp:1603) hands an
# empty image to ops.cpp -> "staging failed / missing image path".
#
# v5 feeds the path with a synthesized WM_DROPFILES instead: main_form.cpp:461 ->
# HandleDroppedFiles(:1328) sets m_wimPath AND pEdit->SetText(:1355) and never
# needs a native child, so the sync timer has nothing to overwrite.
#
# This probe asserts, on the host GUI:
#   1) the drop really reaches HandleDroppedFiles -> the GUI log gains
#      "GUI image loaded: <path>" (main_form.cpp LoadWimImages);
#   2) after the drop the main window still has ZERO native child EDITs, so the
#      sync timer's only source of truth is the control text the drop just set;
#   3) several timer ticks later, clicking MainAction still finds a non-empty
#      m_wimPath -> the Ask3 confirm dialog shows up.
# The confirm is NEVER answered (that would stage a real restore on this host):
# the GUI is killed the moment it appears.
#
# ASCII-only source (PS 5.1 parses no-BOM files as ANSI).
$ErrorActionPreference = 'Continue'
$exe = '<repo>\dist\x64\SysRecoverUI.exe'
$repo = '<repo>'

$cs = @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public class W32 {
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h, uint m, IntPtr w, IntPtr l, uint flags, uint timeout, out IntPtr res);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    // PostDrop: build an HDROP block (DROPFILES = DWORD pFiles(20) + POINT + BOOL
    // fNC + BOOL fWide(1)) followed by the UTF-16 path + double NUL, then post
    // WM_DROPFILES (0x0233) with the block as wParam. The block is leaked on
    // purpose: the GUI reads it asynchronously (DragQueryFileW/DragFinish).
    public static int PostDrop(IntPtr h, string path) {
        int cb = 20 + (path.Length + 2) * 2;
        IntPtr p = Marshal.AllocHGlobal(cb);
        for (int i = 0; i < cb; i++) Marshal.WriteByte(p, i, 0);
        Marshal.WriteInt32(p, 0, 20);
        Marshal.WriteInt32(p, 16, 1);
        byte[] b = Encoding.Unicode.GetBytes(path);
        Marshal.Copy(b, 0, (IntPtr)((long)p + 20), b.Length);
        return PostMessage(h, 0x0233, p, IntPtr.Zero) ? 1 : 0;
    }
}
'@
Add-Type -TypeDefinition $cs -ErrorAction Stop
Write-Host 'Add-Type OK'

function LParam($x, $y) { [IntPtr](($y -shl 16) -bor ($x -band 65535)) }
function ClsName($h) { $sb = New-Object System.Text.StringBuilder 256; [W32]::GetClassName($h, $sb, 256) | Out-Null; $sb.ToString() }
function WinTitle($h) { $sb = New-Object System.Text.StringBuilder 512; [W32]::GetWindowText($h, $sb, 512) | Out-Null; $sb.ToString() }
function IsResponsive($h) {
    $res = [IntPtr]::Zero
    return ([W32]::SendMessageTimeout($h, 0x0000, [IntPtr]::Zero, [IntPtr]::Zero, 3, 3000, [ref]$res)) -ne [IntPtr]::Zero
}
# ClickClient: synchronous click (the return value says whether the GUI thread
# really processed it) plus the same pair posted as a belt-and-braces fallback.
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
function MakeForeground($h) {
    [W32]::ShowWindow($h, 5) | Out-Null
    [W32]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 500
}
$script:guiPid = [uint32]0
function Get-GuiWindows {
    $script:pids = @()
    foreach ($pr in @(Get-Process -Name SysRecoverUI -ErrorAction SilentlyContinue)) { $script:pids += [uint32]$pr.Id }
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
        if (-not [W32]::IsWindowVisible($h)) { return $true }
        $ttl = WinTitle $h
        $rc = New-Object W32+RECT
        [W32]::GetWindowRect($h, [ref]$rc) | Out-Null
        $area = ($rc.R - $rc.L) * ($rc.B - $rc.T)
        $script:winSeen += ($cn + '=' + $ttl + '@' + $rc.L + ',' + $rc.T + ',' + ($rc.R - $rc.L) + 'x' + ($rc.B - $rc.T))
        if ($cn -eq 'SysRecoverUI') { $script:cands += , @($area, $h); return $true }
        return $true
    }
    [W32]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    $bestArea = -1
    foreach ($c in $script:cands) { if ([int]$c[0] -gt $bestArea) { $bestArea = [int]$c[0]; $script:winBest = $c[1] } }
}
function CountChildEdit($h) {
    $script:childSeen = @()
    $cb = [W32+EnumProc]{
        param($ch, $l)
        $script:childSeen += (ClsName $ch)
        return $true
    }
    [W32]::EnumChildWindows($h, $cb, [IntPtr]::Zero) | Out-Null
    return $script:childSeen.Count
}
function GuiLogLines {
    $f = Get-ChildItem -LiteralPath (Join-Path $repo 'dist\logs') -Filter 'SysRecover-*.log' -ErrorAction SilentlyContinue |
         Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $f) { return @() }
    return @(Get-Content -LiteralPath $f.FullName -ErrorAction SilentlyContinue)
}
function GuiLogHits($keys) {
    $lines = GuiLogLines
    $out = @()
    foreach ($k in $keys) { $out += , @($k, @($lines | Where-Object { $_ -like ('*' + $k + '*') }).Count) }
    return $out
}

# ---- driver ----
foreach ($pr in @(Get-Process -Name SysRecoverUI -ErrorAction SilentlyContinue)) {
    Write-Host ('killing leftover GUI pid=' + $pr.Id)
    $pr | Stop-Process -Force -ErrorAction SilentlyContinue
}
$img = (Get-ChildItem -LiteralPath $repo -Filter '*.esd' -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
if (-not $img) { Write-Host 'FAIL: no .esd in repo root to drop'; exit 2 }
Write-Host ('image to drop: ' + $img)
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
$wp = [uint32]0
[W32]::GetWindowThreadProcessId($main, [ref]$wp) | Out-Null
$script:guiPid = $wp
$mcr = New-Object W32+RECT
[W32]::GetClientRect($main, [ref]$mcr) | Out-Null
Write-Host ('main=' + $main + ' pid=' + $wp + ' cls=' + (ClsName $main) + ' title=' + (WinTitle $main) + ' client=' + ($mcr.R - $mcr.L) + 'x' + ($mcr.B - $mcr.T) + ' enabled=' + [W32]::IsWindowEnabled($main))
Write-Host ('pid windows: [' + ($script:winSeen -join '; ') + ']')
# dismiss startup notices (rescue receipt / relocate / leftover scan): these are
# NOT the confirm dialog, which can only appear after MainAction is clicked.
foreach ($w in @($script:winSeen)) {
    if ($w -like 'SysRecoverUI=*') { continue }
    $h = [IntPtr]::Zero
    $cb = [W32+EnumProc]{
        param($hh, $l)
        if ((WinTitle $hh) -ne '' -and (ClsName $hh) -ne 'SysRecoverUI' -and $hh -ne $script:mainH -and [W32]::IsWindowVisible($hh)) {
            $p2 = [uint32]0
            [W32]::GetWindowThreadProcessId($hh, [ref]$p2) | Out-Null
            if ($p2 -eq $script:guiPid) { $script:dlgH = $hh }
        }
        return $true
    }
    $script:mainH = $main
    $script:dlgH = [IntPtr]::Zero
    [W32]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    if ($script:dlgH -ne [IntPtr]::Zero) {
        Write-Host ('closing startup dialog ' + (ClsName $script:dlgH) + '=' + (WinTitle $script:dlgH))
        [W32]::PostMessage($script:dlgH, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        Start-Sleep -Seconds 1
    }
}
$t0 = Get-Date
while (((Get-Date) -lt $t0.AddSeconds(180)) -and (-not (IsResponsive $main))) { Start-Sleep -Seconds 2 }
Start-Sleep -Seconds 4
Write-Host ('responsive=' + (IsResponsive $main))
MakeForeground $main
$n0 = CountChildEdit $main
Write-Host ('child windows BEFORE drop: n=' + $n0 + ' classes=[' + ($script:childSeen -join ' ') + ']')
$hits0 = GuiLogHits @('image loaded')
foreach ($hh in $hits0) { Write-Host ('gui log BEFORE: ' + $hh[0] + '=' + $hh[1]) }

$post1 = [W32]::PostDrop($main, $img)
Write-Host ('WM_DROPFILES(main) posted=' + $post1 + ' path=' + $img)
$loaded = $false
for ($i = 0; $i -lt 20; $i++) {
    Start-Sleep -Seconds 1
    $hits = GuiLogHits @('image loaded')
    if ([int]$hits[0][1] -gt [int]$hits0[0][1]) { $loaded = $true; break }
}
Write-Host ('drop reached HandleDroppedFiles (gui log gained "image loaded")=' + $loaded)
foreach ($h in (GuiLogHits @('image loaded'))) { Write-Host ('gui log AFTER: ' + $h[0] + '=' + $h[1]) }
$n1 = CountChildEdit $main
Write-Host ('child windows AFTER drop: n=' + $n1 + ' classes=[' + ($script:childSeen -join ' ') + ']')
$statusLines = @(GuiLogLines | Where-Object { $_ -like '*GUI status*' } | Select-Object -Last 4)
foreach ($l in $statusLines) { Write-Host ('status: ' + $l) }

# let the 500ms sync timer (main_form.cpp:187/380) run several ticks, then click
# MainAction -- if m_wimPath had been wiped by then, StartRestore (main_form.cpp:1495)
# would only set its "select an image first" status line and no dialog would appear.
Write-Host 'waiting 4s so the path-sync timer ticks at least 8 times'
Start-Sleep -Seconds 4
Write-Host 'clicking MainAction (client 177,318)'
$proc2 = ClickClient $main 177 318
Write-Host ('MainAction processed=' + $proc2)
$cf = [IntPtr]::Zero
for ($i = 0; $i -lt 25; $i++) {
    Start-Sleep -Seconds 1
    if (-not [W32]::IsWindow($main)) { Write-Host 'main window vanished'; break }
    $cand = [IntPtr]::Zero
    $cb = [W32+EnumProc]{
        param($hh, $l)
        if ($hh -eq $script:mainH) { return $true }
        $p2 = [uint32]0
        [W32]::GetWindowThreadProcessId($hh, [ref]$p2) | Out-Null
        if ($p2 -ne $script:guiPid) { return $true }
        if (-not [W32]::IsWindowVisible($hh)) { return $true }
        if ((ClsName $hh) -like 'tooltips*') { return $true }
        $script:foundH = $hh
        return $true
    }
    $script:mainH = $main
    $script:foundH = [IntPtr]::Zero
    [W32]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    if ($script:foundH -ne [IntPtr]::Zero) { $cf = $script:foundH; break }
}
if ($cf -eq [IntPtr]::Zero) {
    Write-Host 'RESULT: no dialog after MainAction (m_wimPath was empty -> guard hit)'
    foreach ($l in (GuiLogLines | Where-Object { $_ -like '*GUI status*' } | Select-Object -Last 4)) { Write-Host ('status after click: ' + $l) }
} else {
    Write-Host ('RESULT: dialog appeared cls=' + (ClsName $cf) + ' title=' + (WinTitle $cf) + ' -> m_wimPath survived the sync timer')
}
Write-Host 'killing GUI (never answer the confirm: that would stage a real restore)'
foreach ($pr in @(Get-Process -Name SysRecoverUI -ErrorAction SilentlyContinue)) { $pr | Stop-Process -Force -ErrorAction SilentlyContinue }
Start-Sleep -Seconds 2
Write-Host ('GUI still running=' + (@(Get-Process -Name SysRecoverUI -ErrorAction SilentlyContinue).Count))
Write-Host 'probe-drop done'
