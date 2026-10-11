# kbd-smoke.ps1 - automated keyboard-shortcut smoke for SysRecoverUI.exe.
#
# IMPORTANT: keep this file ASCII-only. PowerShell 5.1 parses .ps1 as ANSI when
# there is no BOM, so any CJK text in here breaks the parser. param() must be
# the first statement.
#
# Covers (feature 2026-10-10: plain letter + Alt+letter, global Enter,
# Ctrl+Tab mode switch):
#   T1  Alt+N in restore mode  -> NoteInput hidden => focus must NOT move
#   T2  Ctrl+Tab               -> mode switches restore -> backup (pixel diff
#                                 of the tab strip), focus not stolen
#   T3  Alt+N in backup mode   -> focus moves into native EDIT child; typing
#                                 "abc" lands in the note edit (appended)
#   T4  Alt+Q while EDIT focused -> Silent checkbox toggles (pixel diff),
#       no stray 'q' inserted, process alive
#   T4b Alt+T in backup mode     -> FormatBox dropdown (ComboWnd) opens,
#       no stray 't'; posted ESC closes the popup only, process alive
#   T5  click blank area, then Enter with gray MainAction
#                               -> swallowed: no dialog, process alive
#   T6  ESC                    -> window closes, process exits <= 5s
#
# usage: powershell -ExecutionPolicy Bypass -File tools\ui\kbd-smoke.ps1
#        ... -Exe dist\x64\SysRecoverUI.exe -Out <dir>
#
# Exit code: 0 = all checks passed, 1 = at least one failed.
param(
    [string]$Exe = 'D:\Prog\_Project\SysRecover\dist\x64\SysRecoverUI.exe',
    [string]$Out = "$env:TEMP\zj-kbd-smoke",
    [string]$From = 'T1',   # debug: start at this step (T1|T2|T3|T4|T5|T6)
    [switch]$T6Only         # debug: skip T1..T5, just launch + injected ESC
)
$ErrorActionPreference = 'Stop'
if ($T6Only) { $From = 'T6' }
$fromIdx = 0
switch ($From) { 'T1' { $fromIdx = 0 } 'T2' { $fromIdx = 1 } 'T3' { $fromIdx = 2 }
                 'T4' { $fromIdx = 3 } 'T5' { $fromIdx = 4 } 'T6' { $fromIdx = 5 } }
Add-Type -AssemblyName System.Drawing

$sig = @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class KbdSmk {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] public struct GUITHREADINFO {
        public int cbSize; public int flags;
        public IntPtr hwndActive; public IntPtr hwndFocus; public IntPtr hwndCapture;
        public IntPtr hwndMenuOwner; public IntPtr hwndMoveSize; public IntPtr hwndCaret;
        public RECT rcCaret;
    }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool GetGUIThreadInfo(uint id, ref GUITHREADINFO info);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsChild(IntPtr parent, IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, StringBuilder l);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, int data, UIntPtr extra);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern void SwitchToThisWindow(IntPtr h, bool altTab);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vk);
    // --- SendInput (returns #inserted; 0 = blocked) replaces keybd_event so
    // failures are visible instead of silent void ---
    [StructLayout(LayoutKind.Sequential)]
    public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Explicit)]
    public struct INPUT_KB {
        [FieldOffset(0)] public uint type;
        [FieldOffset(8)] public KEYBDINPUT ki;
        // union must be sized like MOUSEINPUT (32B on x64) -> whole INPUT = 40B;
        // SendInput validates cb == sizeof(INPUT), a shorter struct is rejected.
        [FieldOffset(8)] public ulong pad0; [FieldOffset(16)] public ulong pad1;
        [FieldOffset(24)] public ulong pad2; [FieldOffset(32)] public ulong pad3;
    }
    [DllImport("user32.dll")]
    public static extern uint SendInput(uint n, ref INPUT_KB p, int cb);
    public static int SendKey(int vk, bool up) {
        INPUT_KB i = new INPUT_KB();
        i.type = 1; // INPUT_KEYBOARD
        i.ki.wVk = (ushort)vk;
        i.ki.dwFlags = up ? 0x0002u : 0u;
        return (int)SendInput(1, ref i, Marshal.SizeOf(typeof(INPUT_KB)));
    }
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    static uint enumPid; static int enumCount;
    static bool EnumCb(IntPtr h, IntPtr l) {
        uint pid; GetWindowThreadProcessId(h, out pid);
        if (pid == enumPid && IsWindowVisible(h) && GetParent(h) == IntPtr.Zero) enumCount++;
        return true;
    }
    public static int CountTopVisible(uint pid) { enumPid = pid; enumCount = 0; EnumWindows(EnumCb, IntPtr.Zero); return enumCount; }
    // T4b: find a VISIBLE top-level window of pid with the given class name.
    // ComboWnd popups are owned WS_POPUP windows (GetParent = owner != 0), so
    // CountTopVisible deliberately misses them - this enumerator does not
    // filter on GetParent, only on pid + visible + class.
    static uint enumPid2; static IntPtr enumHit;
    static string enumCls;
    static bool EnumClsCb(IntPtr h, IntPtr l) {
        uint pid; GetWindowThreadProcessId(h, out pid);
        if (pid == enumPid2 && IsWindowVisible(h)) {
            StringBuilder sb = new StringBuilder(64);
            GetClassName(h, sb, 64);
            if (string.Equals(sb.ToString(), enumCls, StringComparison.OrdinalIgnoreCase)) enumHit = h;
        }
        return true;
    }
    public static IntPtr FindVisibleClass(uint pid, string cls) {
        enumPid2 = pid; enumCls = cls; enumHit = IntPtr.Zero;
        EnumWindows(EnumClsCb, IntPtr.Zero); return enumHit;
    }
    public static IntPtr GetFocusHwnd(uint tid) {
        GUITHREADINFO gi = new GUITHREADINFO();
        gi.cbSize = Marshal.SizeOf(typeof(GUITHREADINFO));
        if (!GetGUIThreadInfo(tid, ref gi)) return IntPtr.Zero;
        return gi.hwndFocus;
    }
}
'@
if (-not ('KbdSmk' -as [type])) { Add-Type -TypeDefinition $sig }

$script:failCount = 0
function Check([string]$name, [bool]$cond, [string]$detail) {
    if ($cond) {
        Write-Output ("PASS  {0}  ({1})" -f $name, $detail)
    } else {
        Write-Output ("FAIL  {0}  ({1})" -f $name, $detail)
        $script:failCount++
    }
}
function Step([string]$msg) { Write-Output ("---- {0}" -f $msg) }

# ---- keyboard helpers (VK_* bytes) ----
# NOTE: PowerShell variable names are CASE-INSENSITIVE - the table must NOT be
# named $VK while parameters/loop vars are named $vk (the param would shadow
# the table and $VKey.MENU silently becomes $null->0). Table=$VKey, params=$key.
$VKey = @{ LCONTROL=0x11; MENU=0x12; TAB=0x09; RETURN=0x0D; ESCAPE=0x1B
         N=0x4E; Q=0x51; A=0x41; B=0x42; C=0x43; T=0x54 }
function KeyDown([int]$key) {
    $n = [KbdSmk]::SendKey($key, $false)
    if ($n -ne 1) { Write-Output ("WARN  SendInput down vk=0x{0:X} inserted={1}" -f $key, $n) }
}
function KeyUp([int]$key) {
    $n = [KbdSmk]::SendKey($key, $true)
    if ($n -ne 1) { Write-Output ("WARN  SendInput up vk=0x{0:X} inserted={1}" -f $key, $n) }
}
function Send-AltChord([int]$key) {
    Send-Prep
    KeyDown $VKey.MENU; KeyDown $key; Start-Sleep -Milliseconds 60; KeyUp $key; KeyUp $VKey.MENU
    Start-Sleep -Milliseconds 400
}
function Send-CtrlChord([int]$key) {
    Send-Prep
    KeyDown $VKey.LCONTROL; KeyDown $key; Start-Sleep -Milliseconds 60; KeyUp $key; KeyUp $VKey.LCONTROL
    Start-Sleep -Milliseconds 600
}
function Send-Tap([int]$key) {
    Send-Prep
    KeyDown $key; Start-Sleep -Milliseconds 60; KeyUp $key
    Start-Sleep -Milliseconds 300
}

function Get-ForegroundInfo {
    $fg = [KbdSmk]::GetForegroundWindow()
    $fgPid = [uint32]0
    if ($fg -ne [IntPtr]::Zero) { [void][KbdSmk]::GetWindowThreadProcessId($fg, [ref]$fgPid) }
    return "fg=$fg class=" + (Get-WinClass $fg) + " pid=$fgPid"
}
# Runtime state line: foreground owner + focus + stuck-modifier check.
# A stuck Alt/Ctrl (lost key-up during an earlier injection) would silently
# turn every later key into a system hotkey - print it before each step.
function Diag([string]$tag) {
    $fg = [KbdSmk]::GetForegroundWindow()
    $alt = (([KbdSmk]::GetAsyncKeyState(0x12) -band 0x8000) -ne 0)
    $ctl = (([KbdSmk]::GetAsyncKeyState(0x11) -band 0x8000) -ne 0)
    $f = Get-FocusHwnd
    Write-Output ("DIAG  {0}: fgIsGui={1} focus={2} altDown={3} ctrlDown={4}" -f $tag, ($fg -eq $script:guiHwnd), $f, $alt, $ctl)
}
# Background automation shells have no foreground rights (the desktop may
# legitimately be owned by another app, e.g. Notepad++). Try, in order:
#   1. SwitchToThisWindow  - ignores the foreground lock (task-switcher API)
#   2. AttachThreadInput + SetForegroundWindow + BringWindowToTop
#   3. a dummy injected keystroke (grants "received input" eligibility) + retry
# Returns $true only when the GUI window IS the foreground window; callers
# must treat $false as BLOCKED and must NOT inject keys anywhere else.
function Ensure-Foreground([IntPtr]$h) {
    if ([KbdSmk]::GetForegroundWindow() -eq $h) { return $true }
    [KbdSmk]::SwitchToThisWindow($h, $true)
    Start-Sleep -Milliseconds 400
    if ([KbdSmk]::GetForegroundWindow() -eq $h) { return $true }

    $myTid = [KbdSmk]::GetCurrentThreadId()
    $guiTid = $script:guiTid
    [void][KbdSmk]::AttachThreadInput($myTid, $guiTid, $true)
    $ok = $false
    for ($i = 0; $i -lt 5; $i++) {
        [void][KbdSmk]::SetForegroundWindow($h)
        [void][KbdSmk]::BringWindowToTop($h)
        Start-Sleep -Milliseconds 200
        if ([KbdSmk]::GetForegroundWindow() -eq $h) { $ok = $true; break }
    }
    [void][KbdSmk]::AttachThreadInput($myTid, $guiTid, $false)
    if ($ok) { return $true }

    [KbdSmk]::keybd_event(0xA0, 0, 0, [UIntPtr]::Zero)       # fake LSHIFT down/up
    [KbdSmk]::keybd_event(0xA0, 0, 2, [UIntPtr]::Zero)
    [void][KbdSmk]::SetForegroundWindow($h)
    Start-Sleep -Milliseconds 300
    if ([KbdSmk]::GetForegroundWindow() -eq $h) { return $true }
    return $false
}
# Hard gate: never let an injected key reach a foreign foreground window
# (it would type into the user's editor). Blocked sends raise an exception.
function Send-Prep {
    if (-not (Ensure-Foreground $script:guiHwnd)) {
        throw ('BLOCKED: GUI is not foreground (' + (Get-ForegroundInfo) + ') - refusing to inject keys')
    }
}

# ---- focus / text helpers ----
$script:guiTid = 0
function Get-FocusHwnd { [KbdSmk]::GetFocusHwnd($script:guiTid) }
function Get-WinClass([IntPtr]$h) {
    $sb = New-Object System.Text.StringBuilder(256)
    [void][KbdSmk]::GetClassName($h, $sb, 256)
    return $sb.ToString()
}
function Get-EditText([IntPtr]$h) {
    $sb = New-Object System.Text.StringBuilder(2048)
    [void][KbdSmk]::SendMessage($h, 0x000D, [IntPtr]2048, $sb)  # WM_GETTEXT
    return $sb.ToString()
}
function Test-IsChild([IntPtr]$parent, [IntPtr]$h) { [KbdSmk]::IsChild($parent, $h) }

# ---- capture / compare ----
function Get-WinBitmap([IntPtr]$h) {
    $r = New-Object 'KbdSmk+RECT'
    [void][KbdSmk]::GetWindowRect($h, [ref]$r)
    $w = $r.Right - $r.Left
    $ht = $r.Bottom - $r.Top
    $bmp = New-Object System.Drawing.Bitmap($w, $ht)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $blank = $true
    $hdc = $g.GetHdc()
    $pw = [KbdSmk]::PrintWindow($h, $hdc, 2)   # PW_RENDERFULLCONTENT
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
    $g.Dispose()
    return $bmp
}
# Count pixels differing beyond threshold inside a window-pixel rect (default
# step=2 sampling; pass -step 1 for small glyphs - the Silent check mark is
# only ~10x9px, at step=2 it yields ~7 samples, too few to assert on).
# Returns -1 when either bitmap is too small (degenerate capture).
function Count-RegionDiff($b1, $b2, [int]$x1, [int]$y1, [int]$x2, [int]$y2, [int]$step = 2) {
    if ($b1.Width -lt $x2 -or $b2.Width -lt $x2 -or $b1.Height -lt $y2 -or $b2.Height -lt $y2) { return -1 }
    if ($step -lt 1) { $step = 1 }
    $n = 0
    for ($y = $y1; $y -lt $y2; $y += $step) {
        for ($x = $x1; $x -lt $x2; $x += $step) {
            $c1 = $b1.GetPixel($x, $y)
            $c2 = $b2.GetPixel($x, $y)
            if ([Math]::Abs($c1.R - $c2.R) -gt 20 -or [Math]::Abs($c1.G - $c2.G) -gt 20 -or [Math]::Abs($c1.B - $c2.B) -gt 20) { $n++ }
        }
    }
    return $n
}

if (-not (Test-Path -LiteralPath $Out)) { New-Item -ItemType Directory -Path $Out | Out-Null }

# ---- launch ----
Get-Process SysRecoverUI -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
$p = Start-Process -FilePath $Exe -PassThru
if ($p.HasExited) {   # x86 launcher re-exec (PIT-082) - follow the real child
    Start-Sleep -Milliseconds 500
    $child = Get-Process SysRecoverUI -ErrorAction SilentlyContinue | Sort-Object StartTime -Descending | Select-Object -First 1
    if ($child) { $p = $child }
}
$h = [IntPtr]::Zero
for ($i = 0; $i -lt 12; $i++) {
    Start-Sleep -Milliseconds 250
    if ($p.HasExited) { break }
    $p.Refresh()
    if ($p.MainWindowHandle -ne [IntPtr]::Zero) { $h = $p.MainWindowHandle; break }
}
if ($h -eq [IntPtr]::Zero) {
    Write-Output 'FAIL  launch  (window not found)'
    Get-Process SysRecoverUI -ErrorAction SilentlyContinue | Stop-Process -Force
    exit 1
}
$tidTmp = [uint32]0
$script:guiTid = [KbdSmk]::GetWindowThreadProcessId($h, [ref]$tidTmp)
$script:guiHwnd = $h
[void][KbdSmk]::ShowWindow($h, 5)
[void][KbdSmk]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, 0x0045)  # TOPMOST
$fgOk = Ensure-Foreground $h
$guiPid = [uint32]0
[void][KbdSmk]::GetWindowThreadProcessId($h, [ref]$guiPid)
Start-Sleep -Milliseconds 600

# Park the cursor over blank top-bar space (client 560,20: between the title
# and the window buttons at x618+) so no tooltip can pollute captures. NOTE:
# the window is 745px wide - never use an x beyond that (a click would land
# on whatever window sits outside ours and steal foreground).
$pt = New-Object 'KbdSmk+POINT'
$pt.X = 560; $pt.Y = 20
[void][KbdSmk]::ClientToScreen($h, [ref]$pt)
[void][KbdSmk]::SetCursorPos($pt.X, $pt.Y)
Start-Sleep -Milliseconds 500

try {

Step 'T0 baseline: foreground + no startup dialog'
Write-Output ("INFO  {0}" -f (Get-ForegroundInfo))
Diag 'T0'
Check 'T0.foreground' $fgOk ("SetForegroundWindow ok=" + $fgOk)
$n0 = [KbdSmk]::CountTopVisible($guiPid)
Check 'T0.no-dialog' ($n0 -eq 1) ("top-level visible windows=$n0 (expect 1)")
$f0 = Get-FocusHwnd
Check 'T0.focus-main' ($f0 -ne [IntPtr]::Zero -and (-not (Test-IsChild $h $f0))) ("focus=" + $f0 + " class=" + (Get-WinClass $f0))

if ($fromIdx -le 0) {
Step 'T1 Alt+N in restore mode (NoteInput hidden) must NOT move focus'
[void](Ensure-Foreground $h)
Send-AltChord $VKey.N
$f1 = Get-FocusHwnd
$child1 = ($f1 -ne [IntPtr]::Zero) -and (Test-IsChild $h $f1)
Check 'T1.alt-n-hidden' (-not $child1) ("focus after Alt+N=" + $f1 + " class=" + (Get-WinClass $f1) + " child=" + $child1)
}

if ($fromIdx -le 1) {
Step 'T2 Ctrl+Tab switches mode restore -> backup (tab strip pixel diff)'
Diag 'T2-pre'
[void](Ensure-Foreground $h)
$b1 = Get-WinBitmap $h
Send-CtrlChord $VKey.TAB
$b2 = Get-WinBitmap $h
$diff2 = Count-RegionDiff $b1 $b2 0 0 380 56
if ($diff2 -lt 30) {   # keep evidence when the mode did not change
    $b1.Save("$Out\t2-before.png", [System.Drawing.Imaging.ImageFormat]::Png)
    $b2.Save("$Out\t2-after.png", [System.Drawing.Imaging.ImageFormat]::Png)
}
$b1.Dispose(); $b2.Dispose()
Check 'T2.ctrl-tab' ($diff2 -ge 30) ("tab-strip differing samples=$diff2 (expect >=30)")
$f2 = Get-FocusHwnd
Check 'T2.focus-not-stolen' (($f2 -ne [IntPtr]::Zero) -and (-not (Test-IsChild $h $f2))) ("focus=" + $f2)
if ($diff2 -ge 30) {
    $b2s = Get-WinBitmap $h
    $b2s.Save("$Out\t2-backup-mode.png", [System.Drawing.Imaging.ImageFormat]::Png)
    $b2s.Dispose()
}
}   # if ($fromIdx -le 1)

if ($fromIdx -le 2) {
Step 'T3 Alt+N in backup mode focuses the note edit; typing appends'
Diag 'T3-pre'
[void](Ensure-Foreground $h)
Send-AltChord $VKey.N
$f3 = Get-FocusHwnd
$child3 = ($f3 -ne [IntPtr]::Zero) -and (Test-IsChild $h $f3)
$cls3 = Get-WinClass $f3
Check 'T3.alt-n-focus' $child3 ("focus=" + $f3 + " class=" + $cls3)
$t1 = ''
if ($child3) { $t1 = Get-EditText $f3 }
foreach ($key in @($VKey.A, $VKey.B, $VKey.C)) {
    [void](Ensure-Foreground $h)
    Send-Tap $key
}
$t2 = ''
if ($child3 -and $f3 -ne [IntPtr]::Zero) { $t2 = Get-EditText $f3 }
Check 'T3.type-lands' ($t2.EndsWith('abc')) ("text after typing='" + $t2 + "'")
}   # if ($fromIdx -le 2)

if ($fromIdx -le 3) {
Step 'T4 Alt+Q with EDIT focused: toggle Silent, no stray char, stay alive'
Diag 'T4-pre'
[void](Ensure-Foreground $h)
$bA = Get-WinBitmap $h
Send-AltChord $VKey.Q
Start-Sleep -Milliseconds 300
$bB = Get-WinBitmap $h
# step=1: glyph measure ~32 samples (mini-t4 ground truth: bbox 10x9px at
# client (365,313)-(374,321) = the CGlyphCheckBoxUI check mark); step=2
# collapsed that to 7 samples so the old >=30 assert could never fire.
$diff4 = Count-RegionDiff $bA $bB 240 288 480 340 1
$t4 = ''
if ($child3 -and $f3 -ne [IntPtr]::Zero) { $t4 = Get-EditText $f3 }
Check 'T4.silent-toggles' ($diff4 -ge 15) ("Silent-region differing samples=$diff4 (expect >=15)")
Check 'T4.no-stray-char' ($t4 -eq $t2) ("text unchanged=" + ($t4 -eq $t2) + " (now='" + $t4 + "')")
Check 'T4.alive' (-not $p.HasExited) ("hasExited=" + $p.HasExited)
# toggle back: state must return to the exact baseline (mini-t4: round-trip 0)
Send-AltChord $VKey.Q
Start-Sleep -Milliseconds 300
$bC = Get-WinBitmap $h
$diff4r = Count-RegionDiff $bA $bC 240 288 480 340 1
Check 'T4.toggle-roundtrip' ($diff4r -ge 0 -and $diff4r -le 2) ("Silent-region samples after 2nd Alt+Q=$diff4r (expect <=2)")
$bA.Dispose(); $bB.Dispose(); $bC.Dispose()
}   # if ($fromIdx -le 3)

# T4b (feature 2026-10-11): backup-mode FormatBox got shortcut="T".
# Alt+T must open its dropdown (ComboWnd popup) without dropping a stray 't'
# into the NOTE edit; posting ESC straight to the popup must close ONLY the
# popup (UICombo.cpp CComboWnd WM_KEYDOWN/VK_ESCAPE consumes it).
# Probe gotcha: opening the popup steals focus from the native EDIT, which
# self-destructs (UIEdit.cpp CEditWnd::OnKillFocus -> WM_CLOSE) - the T3 hwnd
# is dead afterwards. Re-focus via Alt+N (recreates the native edit from the
# control's stored text) and read THAT hwnd for the stray-char check.
if ($fromIdx -le 3) {
Step 'T4b Alt+T opens FormatBox dropdown; no stray char; ESC closes popup only'
Diag 'T4b-pre'
[void](Ensure-Foreground $h)
Send-AltChord $VKey.T
Start-Sleep -Milliseconds 300
$combo = [KbdSmk]::FindVisibleClass($guiPid, 'ComboWnd')
Check 'T4b.alt-t-opens' ($combo -ne [IntPtr]::Zero) ("ComboWnd=" + $combo)
if ($combo -ne [IntPtr]::Zero) {
    [void][KbdSmk]::PostMessage($combo, 0x0100, [IntPtr]0x1B, [IntPtr]0)  # WM_KEYDOWN ESC
    Start-Sleep -Milliseconds 400
}
$comboAfter = [KbdSmk]::FindVisibleClass($guiPid, 'ComboWnd')
Check 'T4b.esc-closes-popup' ($comboAfter -eq [IntPtr]::Zero) ("ComboWnd after ESC=" + $comboAfter)
[void](Ensure-Foreground $h)
Send-AltChord $VKey.N
Start-Sleep -Milliseconds 300
$f3b = Get-FocusHwnd
$child3b = ($f3b -ne [IntPtr]::Zero) -and (Test-IsChild $h $f3b)
$t4c = ''
if ($child3b) { $t4c = Get-EditText $f3b }
Check 'T4b.focus-back-edit' $child3b ("focus after Alt+N=" + $f3b + " class=" + (Get-WinClass $f3b))
Check 'T4b.no-stray-char' ($t4c -eq $t2) ("text unchanged=" + ($t4c -eq $t2) + " (now='" + $t4c + "')")
Check 'T4b.alive' (-not $p.HasExited) ("hasExited=" + $p.HasExited)
}   # if ($fromIdx -le 3)

if ($fromIdx -le 4) {
Step 'T5 click blank, then Enter with gray MainAction is swallowed'
Diag 'T5-pre'
[void](Ensure-Foreground $h)
    # Click CLIENT blank: y=58 sits between top-bar bottom (h=52) and card1
    # (y=66). y=20 was inside the caption drag zone (caption="0,0,0,36",
    # PIT-015) -> WM_NCLBUTTONDOWN -> no ::SetFocus(m_hWndPaint), so the
    # native EDIT kept OS focus and T5.focus-back-main failed spuriously.
    $pt2 = New-Object 'KbdSmk+POINT'
    $pt2.X = 560; $pt2.Y = 58
[void][KbdSmk]::ClientToScreen($h, [ref]$pt2)
[void][KbdSmk]::SetCursorPos($pt2.X, $pt2.Y)
Start-Sleep -Milliseconds 300
[KbdSmk]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)   # LEFTDOWN
Start-Sleep -Milliseconds 60
[KbdSmk]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)   # LEFTUP
    Start-Sleep -Milliseconds 500
    Diag 'T5-postclick'
    $f5 = Get-FocusHwnd
$child5 = ($f5 -ne [IntPtr]::Zero) -and (Test-IsChild $h $f5)
Check 'T5.focus-back-main' (-not $child5) ("focus=" + $f5 + " class=" + (Get-WinClass $f5))
$n1 = [KbdSmk]::CountTopVisible($guiPid)
[void](Ensure-Foreground $h)
Send-Tap $VKey.RETURN
Start-Sleep -Milliseconds 500
$n2 = [KbdSmk]::CountTopVisible($guiPid)
Check 'T5.enter-swallowed' ($n2 -eq $n1 -and -not $p.HasExited) ("windows before=$n1 after=$n2 hasExited=" + $p.HasExited)
}   # if ($fromIdx -le 4)

Step 'T6 ESC closes the window and exits the process'
Diag 'T6-pre'
[void](Ensure-Foreground $h)
Send-Tap $VKey.ESCAPE
$exited = $false
try { $exited = $p.WaitForExit(5000) } catch { $exited = $p.HasExited }
Check 'T6.esc-exits' ($exited -or $p.HasExited) ("exited=" + $exited)
if (-not ($exited -or $p.HasExited)) {
    Diag 'T6-post-fail'
    # Layered bisect: injected key did not close us. Post the same WM_KEYDOWN
    # straight into the queue - if THAT closes, injection never reached the
    # queue (routing problem); if not, the message pump/state is polluted.
    [void][KbdSmk]::PostMessage($h, 0x0100, [IntPtr]0x1B, [IntPtr]0)  # WM_KEYDOWN ESC
    $posted = $false
    try { $posted = $p.WaitForExit(3000) } catch { $posted = $p.HasExited }
    Write-Output ("BISECT T6.posted-esc-closes={0}" -f $posted)
}

} catch {
    Write-Output ("ABORT  {0}" -f $_.Exception.Message)
    $script:failCount++
} finally {
    Start-Sleep -Milliseconds 300
    Get-Process SysRecoverUI -ErrorAction SilentlyContinue | Stop-Process -Force
}

Write-Output ''
if ($script:failCount -eq 0) {
    Write-Output 'RESULT: ALL PASS'
    exit 0
} else {
    Write-Output ("RESULT: {0} FAILED" -f $script:failCount)
    exit 1
}
