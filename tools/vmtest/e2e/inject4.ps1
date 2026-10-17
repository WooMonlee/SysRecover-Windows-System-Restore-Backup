# inject4.ps1 -- S4 host-side offline injection into dual.raw (system B stage)
# At this point the rescue swap has run: root = system B, old system A is at C:\~old
# with C:\~old\.zj-keepold present (so the app's startup shows the 3-choice
# "keep old system" dialog). This script injects the S5 automation into B:
#   autologon + Run/RunOnce(auto-b) + GPO pre-logon hook (known pw) + C:\zjgui +
#   MARK-B; also clears transient markers (pending-restore).
# No bcdboot/bootsect needed (bootfix already made B bootable) and no dist copy
# (B already has C:\Program Files\SysRecover installed by the A-engine deploy).
# Round-trip: raw -> vpc -> attach -> edit -> detach -> raw. Run elevated, no QEMU.
param(
    [string]$Raw = '',
    [switch]$KeepAttached,
    # S4b leg ("-FirstBoot"): rebuild a *clone-first-boot* state on an
    # already-swapped disk so the user's 2026-10-17 flow can be verified without
    # re-running the multi-hour chain:
    #   1) (re)create the small kept-old tree C:\~old (with .zj-keepold);
    #   2) plant the old log dirs the rescue layer / older builds left on p2
    #      (D:\ZJRESTORE\logs + D:\ZJRESTORE-logs) + D:\ZJRESTORE\bootfix so the
    #      dir survives and its Hidden attribute is checkable;
    #   3) drop the *product's* RunOnce value ZJSysRecoverFirstBoot (the channel
    #      under test) and put zjclick in --no-launch mode so the RunOnce is the
    #      ONLY thing that can start the GUI (clean positive/negative signal).
    # Evidence to read afterwards: "log migrate:" line + planted files in
    # C:\Program Files\SysRecover\logs, GUIs auto-started, ~old gone,
    # D:\ZJRESTORE hidden.
    [switch]$FirstBoot
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Raw) { $Raw = (Resolve-Path (Join-Path $here '..\base\dual.raw')).Path }
$vhd = [IO.Path]::ChangeExtension($Raw, '.vhd')
$qimg = 'D:\Prog\ProgIDE\msys64\mingw64\bin\qemu-img.exe'
$dpTmp = Join-Path $env:TEMP ('zjdp4-' + (Get-Random) + '.txt')
$fail = $false
function Fail($m) { Write-Host "[FAIL] $m" -ForegroundColor Red; $script:fail = $true; throw $m }
function Log($m) { Write-Host "[inject4] $m" }

# Registry.pol REG_DWORD record, byte-for-byte identical to the product's
# ea::PutPolDword() (src/common/ea.cpp) -- keep the two in sync.
function PolDword($out, [string]$key, [string]$name, [uint32]$value) {
    $out.Add([byte]0x5B); $out.Add([byte]0x00)                      # '['
    foreach ($b in [Text.Encoding]::Unicode.GetBytes($key)) { $out.Add($b) }
    $out.Add([byte]0x00); $out.Add([byte]0x00)
    $out.Add([byte]0x3B); $out.Add([byte]0x00)                      # ';'
    foreach ($b in [Text.Encoding]::Unicode.GetBytes($name)) { $out.Add($b) }
    $out.Add([byte]0x00); $out.Add([byte]0x00)
    $out.Add([byte]0x3B); $out.Add([byte]0x00)
    $out.Add([byte]4); $out.Add([byte]0); $out.Add([byte]0); $out.Add([byte]0)   # REG_DWORD
    $out.Add([byte]0x3B); $out.Add([byte]0x00)
    $out.Add([byte]4); $out.Add([byte]0); $out.Add([byte]0); $out.Add([byte]0)   # size = 4
    $out.Add([byte]0x3B); $out.Add([byte]0x00)
    $out.Add([byte]($value -band 0xFF))
    $out.Add([byte](($value -shr 8) -band 0xFF))
    $out.Add([byte](($value -shr 16) -band 0xFF))
    $out.Add([byte](($value -shr 24) -band 0xFF))
    $out.Add([byte]0x5D); $out.Add([byte]0x00)                      # ']'
}

$poolLetters = @('S','T','U','V','W','X','Y','Z','P','Q','R','N','M')

function Run-DiskPart([string]$body) {
    $content = "select vdisk file=`"$script:vhd`"`r`n" + $body + "`r`nexit`r`n"
    Set-Content -LiteralPath $script:dpTmp -Value $content -Encoding ASCII
    $out = & diskpart /s $script:dpTmp 2>&1 | Out-String
    Log ("diskpart rc=$LASTEXITCODE :: " + ($out -replace '\s+', ' ').Trim())
}

# --- preflight ---------------------------------------------------------------
if (Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue) { Fail 'QEMU still running' }
if (-not (Test-Path $Raw)) { Fail "raw missing: $Raw" }
$free = (Get-PSDrive -Name $Raw.Substring(0,1)).Free
Log ("free space on target drive: {0:N1} GB" -f ($free/1GB))

$fs = [IO.File]::OpenRead($Raw); $mbr = New-Object byte[] 512; [void]$fs.Read($mbr,0,512); $fs.Close()
Log ('MBR before: first16=' + (($mbr[0..15] | ForEach-Object { $_.ToString('x2') }) -join ' ') +
     ' active=' + $mbr[446].ToString('x2') + ' sig=' + $mbr[510].ToString('x2'))

# --- cleanup stale attach from a previous failed run -------------------------
$staleLetters = @(); $staleDisk = $null
foreach ($l in $poolLetters) {
    if (-not (Test-Path "${l}:\")) { continue }
    $pt = Get-Partition -DriveLetter $l -ErrorAction SilentlyContinue
    if (-not $pt) { continue }
    $dk = $pt | Get-Disk
    if ($dk.BusType -eq 'File Backed Virtual') { $staleLetters += $l; $staleDisk = $dk.Number }
}
if ($staleLetters.Count -gt 0) {
    Log ("stale VHD attach found (disk {0}, letters {1}) - detaching first" -f $staleDisk, ($staleLetters -join ','))
    $body = ''
    foreach ($l in $staleLetters) { $body += "select volume ${l}`r`nremove letter=${l}`r`n" }
    $body += 'detach vdisk'
    Run-DiskPart $body
    foreach ($l in $staleLetters) { if (Test-Path "${l}:\") { Fail "stale letter ${l}: still mounted after detach" } }
    Log 'stale attach detached OK'
}

# --- convert raw -> vpc (de-sparsify by stream copy) -------------------------
Log 'convert raw -> vpc (then de-sparsify by stream copy)'
$part = $vhd + '.part'
& $qimg convert -f raw -O vpc $Raw $part
if ($LASTEXITCODE -ne 0) { Fail 'qemu-img convert raw->vpc failed' }
$s = [IO.File]::OpenRead($part); $d = [IO.File]::Create($vhd)
$buf = New-Object byte[] (64MB)
try { while (($n = $s.Read($buf, 0, $buf.Length)) -gt 0) { $d.Write($buf, 0, $n) } }
finally { $d.Close(); $s.Close() }
Remove-Item -LiteralPath $part -Force
Log ("vhd ready, {0:N1} MB" -f ((Get-Item $vhd).Length/1MB))

# --- pick free letters + attach ---------------------------------------------
$letters = $poolLetters |
    Where-Object { -not (Test-Path ("{0}:\" -f $_)) } | Select-Object -First 2
if ($letters.Count -lt 2) { Fail 'no free drive letters' }
$L1 = $letters[0]; $L2 = $letters[1]
Log ("letters: p1=$L1 p2=$L2")
Log 'attach vhd'
Run-DiskPart "attach vdisk`r`nselect partition 1`r`nassign letter=$L1`r`nselect partition 2`r`nassign letter=$L2"

# --- guards: this must be system B at root, with A kept at ~old --------------
if (-not (Test-Path "${L1}:\Windows\System32\winload.exe")) { Fail "attach/assign failed (no system at L1=$L1)" }
if (Test-Path "${L1}:\~new") { Fail '~new still present - swap did not run (S3b missing?)' }
if ($FirstBoot -and -not (Test-Path "${L1}:\~old")) {
    # -FirstBoot: the S5 leg already deleted the real ~old. Rebuild a *small*
    # one that satisfies every consumer of the post-swap state:
    #   .zj-keepold   -> StartupPhase() keeps the tree + GUI shows the 3-choice
    #                    "kept old system" dialog (that is the flow under test)
    #   Windows\...   -> ProbeDrive() rootIntact / inject4 guards
    #   MARK-A.txt    -> inject4 guard (proof ~old is the real A tree)
    # Small on purpose: the delete under test must finish in seconds.
    $old = "${L1}:\~old"
    foreach ($d in "$old", "$old\Windows\System32", "$old\Users\Public\Desktop",
                   "$old\Users\Administrator\Documents") {
        New-Item -ItemType Directory -Path $d -Force | Out-Null
    }
    Set-Content -LiteralPath "$old\.zj-keepold" -Value 'zj-e2e fake kept-old marker' -Encoding ASCII
    Set-Content -LiteralPath "$old\Windows\System32\winload.exe" -Value 'fake winload (harness)' -Encoding ASCII
    Set-Content -LiteralPath "$old\Users\Public\Desktop\MARK-A.txt" -Value 'zj-e2e marker A (fake kept-old)' -Encoding ASCII
    1..3 | ForEach-Object { Set-Content -LiteralPath "$old\Users\Administrator\Documents\zjfake$_.dat" -Value ("fake file " + $_) -Encoding ASCII }
    Log 'FirstBoot: fake ~old tree created (marker + winload + MARK-A + 3 files)'
}
if (-not (Test-Path "${L1}:\~old\Windows\System32\winload.exe")) { Fail '~old missing - not the post-swap state' }
if (-not (Test-Path "${L1}:\~old\.zj-keepold")) { Fail '~old\.zj-keepold missing - app would show the 2-choice dialog' }
if (-not (Test-Path "${L1}:\~old\Users\Public\Desktop\MARK-A.txt")) { Fail '~old MARK-A missing' }
if (-not (Test-Path "${L1}:\Program Files\SysRecover\SysRecoverUI.exe")) { Fail 'system B program not deployed at root' }
foreach ($f in 'skin\main.xml', 'lang\en.lang', 'bootfiles\vmlinuz-zjrestore') {
    if (-not (Test-Path "${L1}:\Program Files\SysRecover\$f")) { Fail "installed program incomplete: $f" }
}
if (-not (Test-Path "${L2}:\images\c.esd")) { Fail "p2 is not the ZJIMG data partition (L2=$L2)" }
Log 'guards OK (system B at root, A at ~old, keepold marker present)'

# --- sync the installed program with the current dist build ------------------
# The guest image ships an older SysRecoverUI.exe: builds <= 0.7.56 free an
# interior DACL pointer in swapleft.cpp::TakeOwnershipAndGrant -> the delete
# path dies with STATUS_HEAP_CORRUPTION (0xC0000374) as soon as it starts.
# S5 must therefore run the *fixed* build; both the x86 entry and the x64 exe
# (the entry re-execs the latter) plus the shared skin/lang are refreshed.
$distRoot = (Resolve-Path (Join-Path $here '..\..\..\dist')).Path
$appRoot  = "${L1}:\Program Files\SysRecover"
$sync = @(
    @{ S = "$distRoot\SysRecoverUI.exe";     D = "$appRoot\SysRecoverUI.exe" },
    @{ S = "$distRoot\libwim-15.dll";        D = "$appRoot\libwim-15.dll" },
    @{ S = "$distRoot\x64\SysRecoverUI.exe"; D = "$appRoot\x64\SysRecoverUI.exe" },
    @{ S = "$distRoot\x64\libwim-15.dll";    D = "$appRoot\x64\libwim-15.dll" })
foreach ($p in $sync) {
    if (-not (Test-Path $p.S)) { Fail "dist artifact missing: $($p.S)" }
    $dd = Split-Path -Parent $p.D
    if (-not (Test-Path $dd)) { New-Item -ItemType Directory -Path $dd -Force | Out-Null }
    Copy-Item -LiteralPath $p.S -Destination $p.D -Force
    Log ("synced {0} -> {1} ({2} bytes)" -f (Split-Path -Leaf $p.S), (Split-Path -Leaf $dd), (Get-Item -LiteralPath $p.D -Force).Length)
}
foreach ($sub in 'lang', 'skin') {
    if (Test-Path (Join-Path $distRoot $sub)) {
        Copy-Item -Path (Join-Path $distRoot "$sub\*") -Destination "$appRoot\$sub\" -Recurse -Force
        Log "synced $sub\"
    }
}
$verJson = "$distRoot\version.json"
if (Test-Path $verJson) { Log ("dist version: " + ([IO.File]::ReadAllText($verJson)).Trim()) }
if ((Get-Item -LiteralPath "$appRoot\x64\SysRecoverUI.exe" -Force).Length -ne (Get-Item -LiteralPath "$distRoot\x64\SysRecoverUI.exe" -Force).Length) { Fail 'x64 exe not synced' }
Log 'installed program synced with dist'

# --- clear transient markers from the S3 leg ---------------------------------
# (a stale restore-task.conf can make a later rescue boot pick the wrong path;
#  a stale pending-restore marker would pop the "task not executed" notice at
#  B's first boot and confuse the automation dialog sequence)
foreach ($f in "${L1}:\restore-task.conf", "${L1}:\restore-task.json",
               "${L2}:\restore-task.conf", "${L2}:\restore-task.json",
               "${L2}:\ZJRESTORE\logs\pending-restore.txt") {
    if (Test-Path $f) { Remove-Item -LiteralPath $f -Force -ErrorAction SilentlyContinue; Log "removed transient: $f" }
}

# --- FirstBoot: plant the old-log state + a clean destination ----------------
# The flow under test (user 2026-10-17 spec) is: the FIRST boot of the new system
# auto-runs the app, which moves whatever the rescue layer / older builds left on
# the data drive into <program>\logs, and only then asks about ~old. Plant exactly
# that input state; the destination is cleared so "what moved" is unambiguous.
if ($FirstBoot) {
    $dstLogs = "${L1}:\Program Files\SysRecover\logs"
    if (Test-Path $dstLogs) {
        Get-ChildItem -LiteralPath $dstLogs -Force | Remove-Item -Recurse -Force
        Log "FirstBoot: cleared app log dir ($dstLogs)"
    }
    # Keep ZJRESTORE itself non-empty (real state has bootfix\ + build stamp) so
    # the dir survives the move and its Hidden attribute can be verified.
    New-Item -ItemType Directory -Path "${L2}:\ZJRESTORE\bootfix" -Force | Out-Null
    Set-Content -LiteralPath "${L2}:\ZJRESTORE\bootfix\BCD" -Value 'fake bootfix BCD (harness)' -Encoding ASCII
    Set-Content -LiteralPath "${L2}:\ZJRESTORE\rescue-build.txt" -Value 'harness' -Encoding ASCII
    $srcA = "${L2}:\ZJRESTORE\logs"          # PIT-059 software-dir fallback
    New-Item -ItemType Directory -Path $srcA -Force | Out-Null
    Get-ChildItem -LiteralPath $srcA -Force | Remove-Item -Recurse -Force
    Set-Content -LiteralPath "$srcA\zj-demo-migrate.log" -Value 'PLANTED-LOG-A <drv>:\ZJRESTORE\logs' -Encoding ASCII
    $srcB = "${L2}:\ZJRESTORE-logs"          # PIT-137 black-box home
    if (Test-Path $srcB) { Remove-Item -LiteralPath $srcB -Recurse -Force }
    New-Item -ItemType Directory -Path $srcB -Force | Out-Null
    Set-Content -LiteralPath "$srcB\ZJRESTORE-last.log" -Value 'PLANTED-LOG-B <drv>:\ZJRESTORE-logs' -Encoding ASCII
    Set-Content -LiteralPath "$srcB\ZJRESTORE-status.txt" -Value 'PLANTED-LOG-C (in ZJRESTORE-logs, NOT a root receipt)' -Encoding ASCII
    foreach ($f in "$srcA\zj-demo-migrate.log", "$srcB\ZJRESTORE-last.log",
                   "$srcB\ZJRESTORE-status.txt", "${L2}:\ZJRESTORE\bootfix\BCD") {
        if (-not (Test-Path $f)) { Fail "FirstBoot plant failed: $f" }
    }
    Log ("FirstBoot: planted 3 log files (2 sources) on $L2 + bootfix\BCD")
}

# --- registry: autologon + RunOnce(auto-b) + UAC off ------------------------
$AutoPw = 'zjtest'
$reg = "$env:SystemRoot\System32\reg.exe"
$sp = "${L1}:\Windows\System32"
Log 'load SOFTWARE hive'
& $reg load HKLM\ZJSYS "$sp\config\SOFTWARE"
if ($LASTEXITCODE -ne 0) { Fail "reg load rc=$LASTEXITCODE" }
try {
    $excl = @('Public','Default','Default User','defaultuser0','All Users','WDAGUtilityAccount')
    $cands = @()
    foreach ($d in (Get-ChildItem "${L1}:\Users" -Directory -Force -ErrorAction SilentlyContinue)) {
        if ($d.Name -in $excl) { continue }
        $nu = Join-Path $d.FullName 'NTUSER.DAT'
        if (-not (Test-Path $nu)) { continue }
        $t = $null
        try { $t = (Get-Item -LiteralPath $nu -Force -ErrorAction Continue).LastWriteTime } catch { $t = $null }
        $cands += [pscustomobject]@{ Name = $d.Name; T = $t }
    }
    $pick = $cands | Sort-Object T -Descending | Select-Object -First 1
    if ($pick -and $pick.Name) {
        $user = $pick.Name
        Log ("user: " + $user + " (candidates: " + (($cands | ForEach-Object { "{0}@{1:HH:mm}" -f $_.Name, $_.T }) -join ',') + ")")
    } else {
        $user = 'Administrator'
        Log 'no user profile found -> user=Administrator'
    }

    $wl = 'HKLM\ZJSYS\Microsoft\Windows NT\CurrentVersion\Winlogon'
    & $reg add $wl /v AutoAdminLogon   /t REG_SZ /d 1       /f; if ($LASTEXITCODE) { Fail 'add AutoAdminLogon' }
    & $reg add $wl /v DefaultUserName  /t REG_SZ /d $user   /f; if ($LASTEXITCODE) { Fail 'add DefaultUserName' }
    & $reg add $wl /v DefaultPassword  /t REG_SZ /d $AutoPw /f; if ($LASTEXITCODE) { Fail 'add DefaultPassword' }
    & $reg add $wl /v DisableCAD       /t REG_SZ /d 1       /f; if ($LASTEXITCODE) { Fail 'add DisableCAD' }
    # zjautob = the harness driver. In -FirstBoot mode it must NOT start the GUI
    # itself: then the product's RunOnce value (below) is the only possible GUI
    # starter -> "GUI came up" is proof the channel works, and "never came up" is
    # unambiguous (zjclick just times out with ~old present).
    #
    # Where it lives matters! The shell processes RunOnce entries *sequentially
    # and waits for each program to exit*: a long-running zjclick parked in the
    # same RunOnce key head-of-line-blocks the product's own RunOnce value, so the
    # GUI never starts (observed on the first attempt: zjclick ticked for 393s
    # while the product's value sat unprocessed). Run keys are launched
    # asynchronously, so in -FirstBoot mode zjautob goes to \Run instead.
    $zjCmd = 'C:\zjgui\zjclick.exe --secs 3600'
    $runKey = 'HKLM\ZJSYS\Microsoft\Windows\CurrentVersion\RunOnce'
    if ($FirstBoot) {
        $zjCmd = 'C:\zjgui\zjclick.exe --secs 600 --no-launch'
        $runKey = 'HKLM\ZJSYS\Microsoft\Windows\CurrentVersion\Run'
        # Delete via .NET, not reg.exe: with $ErrorActionPreference='Stop', a
        # native tool writing "value not found" to stderr becomes a *terminating*
        # PowerShell error even with 2>$null (PS 5.1 NativeCommandError).
        $kd = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey(
              'ZJSYS\Microsoft\Windows\CurrentVersion\RunOnce', $true)
        if ($kd) { $kd.DeleteValue('zjautob', $false); $kd.Close() }
    }
    & $reg add $runKey /v zjautob /t REG_SZ /d $zjCmd /f
    if ($LASTEXITCODE) { Fail 'add zjautob' }
    if ($FirstBoot) {
        # Drop the product's first-boot RunOnce value (ops.cpp::InjectOfflineRunOnce
        # writes ZJFIRSTBOOT/ZJSysRecoverFirstBoot, REG_EXPAND_SZ, quoted because
        # the path has spaces). .NET API on purpose: reg.exe mangles embedded
        # double quotes; then read the value back and assert byte-exact.
        $fbVal = '"%SystemDrive%\Program Files\SysRecover\SysRecoverUI.exe"'
        $k = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey(
                 'ZJSYS\Microsoft\Windows\CurrentVersion\RunOnce', $true)
        if (-not $k) { Fail 'RunOnce key missing under HKLM\ZJSYS' }
        $k.SetValue('ZJSysRecoverFirstBoot', $fbVal,
                    [Microsoft.Win32.RegistryValueKind]::ExpandString)
        $k.Close()
        $k2 = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey(
                  'ZJSYS\Microsoft\Windows\CurrentVersion\RunOnce', $false)
        # NOTE: GetValue() *expands* REG_EXPAND_SZ by default, so ask for the
        # stored bytes; comparing the expanded form would false-fail (and would
        # use the host's %SystemDrive%). The raw, unexpanded form is what the
        # product stores (ops.cpp::InjectOfflineRunOnce) and what RunOnce expands
        # at logon on the guest.
        $got = $k2.GetValue('ZJSysRecoverFirstBoot', $null,
                            [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        $kind = $k2.GetValueKind('ZJSysRecoverFirstBoot')
        $k2.Close()
        if ($got -ne $fbVal) { Fail ("ZJSysRecoverFirstBoot mismatch: [" + $got + "]") }
        if ("$kind" -ne 'ExpandString') { Fail "ZJSysRecoverFirstBoot kind=$kind (want ExpandString)" }
        Log ("FirstBoot: RunOnce ZJSysRecoverFirstBoot = " + $got + " (" + $kind + ")")
        # zjautob must be async (\Run) here, and must NOT linger in RunOnce --
        # otherwise it head-of-line-blocks the value under test (see note above).
        $k3 = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey(
              'ZJSYS\Microsoft\Windows\CurrentVersion\Run', $false)
        if (-not $k3) { Fail 'FirstBoot: Run key missing' }
        $zj = $k3.GetValue('zjautob'); $k3.Close()
        if ("$zj" -notmatch 'zjclick') { Fail ("FirstBoot: Run\zjautob wrong: [" + $zj + "]") }
        $k4 = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey(
              'ZJSYS\Microsoft\Windows\CurrentVersion\RunOnce', $false)
        if ($k4) {
            $stale = $k4.GetValue('zjautob'); $k4.Close()
            if ("$stale" -match 'zjclick') { Fail ("FirstBoot: zjautob still in RunOnce: [" + $stale + "]") }
        }
        Log ("FirstBoot: Run zjautob = " + $zj + " (async; absent from RunOnce)")
    }
    $psys = 'HKLM\ZJSYS\Microsoft\Windows\CurrentVersion\Policies\System'
    & $reg add $psys /v EnableLUA                  /t REG_DWORD /d 0 /f; if ($LASTEXITCODE) { Fail 'add EnableLUA' }
    & $reg add $psys /v ConsentPromptBehaviorAdmin /t REG_DWORD /d 0 /f; if ($LASTEXITCODE) { Fail 'add Consent' }
} finally {
    & $reg unload HKLM\ZJSYS
    if ($LASTEXITCODE) { Log "WARN: reg unload rc=$LASTEXITCODE" }
}
$rzWhere = 'RunOnce'
if ($FirstBoot) { $rzWhere = 'Run' }
Log "registry OK (autologon + $rzWhere zjautob + UAC off)"

# --- GPO pre-logon hook: set a KNOWN password for the chosen account ---------
# Same channel as inject1 (PIT-122/123/131): a Machine\Scripts\Startup local GPO
# runs as SYSTEM before the logon UI -> sets the account password to $AutoPw and
# rewrites autologon, so the same boot logs on unattended and consumes RunOnce.
$gpRoot  = "${L1}:\Windows\System32\GroupPolicy"
$gpMach  = "$gpRoot\Machine"
$gpStart = "$gpMach\Scripts\Startup"
New-Item -ItemType Directory -Path $gpStart -Force | Out-Null

$startupCmd = "$gpStart\zjlogonb.cmd"
$cmdText = @'
@echo off
rem zj-e2e S5 hook: set a known password + autologon for system B.
set LOG=%SystemDrive%\zjgui\zjlogonb.log
echo [%DATE% %TIME%] zjlogonb start > "%LOG%"
net user @USER@ @PW@ >> "%LOG%" 2>&1
net user @USER@ /active:yes >> "%LOG%" 2>&1
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v AutoAdminLogon /t REG_SZ /d 1 /f >> "%LOG%" 2>&1
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v DefaultUserName /t REG_SZ /d @USER@ /f >> "%LOG%" 2>&1
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v DefaultDomainName /t REG_SZ /d %COMPUTERNAME% /f >> "%LOG%" 2>&1
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v DefaultPassword /t REG_SZ /d @PW@ /f >> "%LOG%" 2>&1
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v DisableCAD /t REG_SZ /d 1 /f >> "%LOG%" 2>&1
echo [%DATE% %TIME%] zjlogonb done >> "%LOG%"
exit /b 0
'@
$cmdText = ($cmdText.Replace('@USER@', $user).Replace('@PW@', $AutoPw)) -replace "`r?`n", "`r`n"
[IO.File]::WriteAllText($startupCmd, $cmdText, [Text.Encoding]::ASCII)

# Fallback leg (still SYSTEM, still pre-logon): if no zjclick instance has claimed
# the run within ~240s (e.g. autologon did not fire on this trimmed image), run
# zjclick.exe from this SYSTEM context. zjclick claims first, so only one wins.
# NOTE: native exe on purpose - this lite image cannot run Add-Type at all
# ("Metadata file 'System.dll' could not be found"), so no PowerShell here.
$startupCmd2 = "$gpStart\zjautoboot.cmd"
$bootText = @'
@echo off
rem zj-e2e S5 fallback: run zjclick as SYSTEM if no interactive instance claimed.
set LOG=%SystemDrive%\zjgui\zjautoboot.log
echo [%DATE% %TIME%] zjautoboot start > "%LOG%"
ping -n 240 127.0.0.1 > nul
if exist C:\zjgui\auto-b.claimed (
  echo [%DATE% %TIME%] claimed elsewhere; skip >> "%LOG%"
  exit /b 0
)
echo [%DATE% %TIME%] no claim - zjclick as SYSTEM >> "%LOG%"
C:\zjgui\zjclick.exe --secs 3600 >> "%LOG%" 2>&1
echo [%DATE% %TIME%] zjclick finished exit=%ERRORLEVEL% >> "%LOG%"
exit /b 0
'@
$bootText = $bootText -replace "`r?`n", "`r`n"
[IO.File]::WriteAllText($startupCmd2, $bootText, [Text.Encoding]::ASCII)

$iniPath = "$gpMach\Scripts\scripts.ini"
function EnsureStartupEntry([string]$name) {
    $iniTxt = ''
    if (Test-Path $iniPath) { $iniTxt = ([IO.File]::ReadAllText($iniPath)) -replace "`r", '' }
    if ($iniTxt -match ([regex]::Escape($name))) { Log ("gpo: scripts.ini already has " + $name + " (left as-is)"); return }
    $idx = 0
    if ($iniTxt.Trim()) {
        $nums = @([regex]::Matches($iniTxt, '(?m)^([0-9]+)') | ForEach-Object { [int]$_.Groups[1].Value })
        if ($nums.Count -gt 0) { $idx = ([int[]]$nums | Measure-Object -Maximum).Maximum + 1 }
        $lines = @($iniTxt -split "`n")
        $ins   = @("$idx`cmdLine=$name", "$idx`Parameters=")
        $st = -1; $en = $lines.Count
        for ($i = 0; $i -lt $lines.Count; $i++) {
            if ($st -lt 0) { if ($lines[$i] -match '^\[Startup') { $st = $i } }
            elseif ($lines[$i] -match '^\[') { $en = $i; break }
        }
        if ($st -lt 0) {
            $lines = $lines + @('[Startup]') + $ins
        } else {
            $head = @(); if ($en -gt 0) { $head = $lines[0..($en - 1)] }
            $tail = @(); if ($en -lt $lines.Count) { $tail = $lines[$en..($lines.Count - 1)] }
            $lines = $head + $ins + $tail
        }
    } else {
        $lines = @('[Startup]', "$idx`cmdLine=$name", "$idx`Parameters=")
    }
    [IO.File]::WriteAllText($iniPath, (($lines -join "`r`n") + "`r`n"), [Text.Encoding]::ASCII)
    Log "gpo: scripts.ini written ($name idx=$idx)"
}
EnsureStartupEntry 'zjlogonb.cmd'
EnsureStartupEntry 'zjautoboot.cmd'

$scrPair = '{42B5FAAE-6536-11D2-AE5A-0000F87571E3}{40B6664F-4972-11D1-A7CA-0000F87571E3}'
$regPair = '{35378EAC-683F-11D2-A89A-00C04FBBCFA2}{D02B1F72-3407-48AE-BA88-E8213C6761F1}'
$gptPath = "$gpRoot\gpt.ini"
$newVer  = 2147418112 + [int]([DateTimeOffset]::UtcNow.ToUnixTimeSeconds() % 32768)
$gIn = @()
if (Test-Path $gptPath) {
    $gRaw = [IO.File]::ReadAllText($gptPath)
    $gIn  = @(($gRaw -replace "`r", '') -split "`n")
    $m = [regex]::Match($gRaw, '(?m)^Version=(\d+)')
    if ($m.Success -and [int64]$m.Groups[1].Value -eq $newVer) { $newVer = $newVer + 1 }
}
$gOut = New-Object 'System.Collections.Generic.List[string]'
$haveGen = $false; $haveExt = $false
foreach ($ln in $gIn) {
    if ($ln -eq '') { continue }
    if ($ln -match '^\[General') { $haveGen = $true; $null = $gOut.Add($ln); continue }
    if ($ln -match '^gPCMachineExtensionNames=') {
        $haveExt = $true
        $v = $ln.Substring($ln.IndexOf('=') + 1)
        if ($v -notlike "*$scrPair*") { $v += "[$scrPair]" }
        if ($v -notlike "*$regPair*") { $v += "[$regPair]" }
        $null = $gOut.Add("gPCMachineExtensionNames=$v"); continue
    }
    if ($ln -match '^Version=') { continue }
    $null = $gOut.Add($ln)
}
if (-not $haveExt) { $null = $gOut.Add("gPCMachineExtensionNames=[$scrPair][$regPair]") }
$null = $gOut.Add("Version=$newVer")
if (-not $haveGen) { $gOut.Insert(0, '[General]') }
[IO.File]::WriteAllText($gptPath, (($gOut -join "`r`n") + "`r`n"), [Text.Encoding]::ASCII)
Log "gpo: gpt.ini written (Version=$newVer)"

$polPath = "$gpMach\Registry.pol"
$frag = New-Object 'System.Collections.Generic.List[byte]'
foreach ($b in [byte[]](0x50, 0x52, 0x65, 0x67, 1, 0, 0, 0)) { $frag.Add($b) }
PolDword $frag 'Software\Microsoft\Windows\CurrentVersion\Policies\System'      'RunStartupScriptSync' 1
PolDword $frag 'Software\Policies\Microsoft\Windows NT\CurrentVersion\Winlogon' 'SyncForegroundPolicy'  1
if (Test-Path $polPath) {
    $pol = New-Object 'System.Collections.Generic.List[byte]'
    foreach ($b in [IO.File]::ReadAllBytes($polPath)) { $pol.Add($b) }
    for ($i = 8; $i -lt $frag.Count; $i++) { $pol.Add($frag[$i]) }
    [IO.File]::WriteAllBytes($polPath, $pol.ToArray())
    Log ("gpo: Registry.pol merged ({0} bytes)" -f $pol.Count)
} else {
    [IO.File]::WriteAllBytes($polPath, $frag.ToArray())
    Log ("gpo: Registry.pol created ({0} bytes)" -f $frag.Count)
}

foreach ($f in $startupCmd, $iniPath, $gptPath, $polPath) {
    if (-not (Test-Path $f)) { Fail "GPO artifact missing: $f" }
    if ((Get-Item -LiteralPath $f -Force).Length -le 0) { Fail "GPO artifact empty: $f" }
}
$pb = [IO.File]::ReadAllBytes($polPath)
if ($pb.Length -lt 8 -or [Text.Encoding]::ASCII.GetString($pb, 0, 4) -ne 'PReg') { Fail 'Registry.pol magic wrong' }
$ptxt = [Text.Encoding]::Unicode.GetString($pb)
if ($ptxt -notmatch 'RunStartupScriptSync') { Fail 'Registry.pol missing RunStartupScriptSync' }
if ($ptxt -notmatch 'SyncForegroundPolicy') { Fail 'Registry.pol missing SyncForegroundPolicy' }
if (([IO.File]::ReadAllText($startupCmd)) -notmatch 'net user') { Fail 'zjlogonb.cmd content wrong' }
if (([IO.File]::ReadAllText($startupCmd2)) -notmatch 'zjclick') { Fail 'zjautoboot.cmd content wrong' }
if (([IO.File]::ReadAllText($iniPath)) -notmatch 'zjautoboot') { Fail 'scripts.ini missing zjautoboot' }
Log ("scripts.ini = " + (([IO.File]::ReadAllText($iniPath)) -replace "`r`n", ' | ').Trim())
Log "GPO pre-logon hook OK (SYSTEM sets $user pw=$AutoPw + autologon before logon UI)"

# --- copy automation + marker ------------------------------------------------
New-Item -ItemType Directory -Path "${L1}:\zjgui" -Force | Out-Null
Copy-Item (Join-Path $here 'zjclick.exe') "${L1}:\zjgui\" -Force
if (-not (Test-Path "${L1}:\zjgui\zjclick.exe")) { Fail 'missing zjgui\zjclick.exe' }
if ((Get-Item -LiteralPath "${L1}:\zjgui\zjclick.exe" -Force).Length -lt 20000) { Fail 'zjclick.exe too small' }
foreach ($f in 'auto-b.cmd', 'auto-b.ps1', 'auto-b.claimed', 'auto-b.log', 'zjautoboot.log', 'zjlogonb.log') {
    $p = "${L1}:\zjgui\$f"
    if (Test-Path $p) { Remove-Item -LiteralPath $p -Force -ErrorAction SilentlyContinue; Log "cleared stale: $f" }
}
# zjclick writes a copy of its log to the ZJIMG partition root too (log_open_all):
# stale copies there would look like fresh evidence after the run.
foreach ($f in "${L2}:\auto-b.log", "${L2}:\auto-a.log") {
    if (Test-Path $f) { Remove-Item -LiteralPath $f -Force -ErrorAction SilentlyContinue; Log "cleared stale: $f" }
}
Log 'zjgui automation OK (zjclick.exe)'
Set-Content -LiteralPath "${L1}:\Users\Public\Desktop\MARK-B.txt" -Value 'zj-e2e marker B (post-swap)' -Encoding ASCII
if (-not (Test-Path "${L1}:\Users\Public\Desktop\MARK-B.txt")) { Fail 'MARK-B write failed' }
Log 'MARK-B written'

# --- detach + convert back ---------------------------------------------------
if ($KeepAttached) { Log 'KeepAttached: leaving disks mounted'; exit 0 }
Log 'detach'
Run-DiskPart "select partition 1`r`nremove letter=$L1`r`nselect partition 2`r`nremove letter=$L2`r`ndetach vdisk"

Log 'convert vpc -> raw (tmp + swap)'
$tmpRaw = $Raw + '.tmp'
& $qimg convert -f vpc -O raw -S 64k $vhd $tmpRaw
if ($LASTEXITCODE -ne 0) { Fail 'qemu-img convert vpc->raw failed' }
Move-Item -LiteralPath $tmpRaw -Destination $Raw -Force

$fs = [IO.File]::OpenRead($Raw); $mbr = New-Object byte[] 512; [void]$fs.Read($mbr,0,512); $fs.Close()
Log ('MBR after : first16=' + (($mbr[0..15] | ForEach-Object { $_.ToString('x2') }) -join ' ') +
     ' active=' + $mbr[446].ToString('x2') + ' sig=' + $mbr[510].ToString('x2'))
if ($mbr[510] -ne 0x55 -or $mbr[511] -ne 0xAA) { Fail 'MBR signature lost in round-trip' }
if ($mbr[446] -ne 0x80) { Fail 'p1 active flag lost' }
Log 'round-trip verify OK'
Remove-Item -LiteralPath $vhd -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $dpTmp -Force -ErrorAction SilentlyContinue
Log 'DONE - S4 injection complete'
exit 0
