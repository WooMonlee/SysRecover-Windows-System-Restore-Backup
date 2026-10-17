# inject1.ps1 -- S1.5 host-side offline injection into dual.raw (system A stage)
# round-trip: raw -> vpc -> diskpart attach -> bcdboot + bootsect + autologon
#              + GPO pre-logon startup script (set KNOWN Administrator pw)
#              + RunOnce(auto-a) + copy dist -> S:\zjgui + MARK-A -> detach -> raw
# Run elevated. QEMU must NOT be running.
param(
    [string]$Raw = '',
    [string]$Dist = '',
    [switch]$KeepAttached
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Raw)   { $Raw   = (Resolve-Path (Join-Path $here '..\base\dual.raw')).Path }
if (-not $Dist)  { $Dist  = (Resolve-Path (Join-Path $here '..\..\..')).Path + '\dist' }
$vhd = [IO.Path]::ChangeExtension($Raw, '.vhd')
$qimg = 'D:\Prog\ProgIDE\msys64\mingw64\bin\qemu-img.exe'
$dpTmp = Join-Path $env:TEMP ('zjdp-' + (Get-Random) + '.txt')
$fail = $false
function Fail($m) { Write-Host "[FAIL] $m" -ForegroundColor Red; $script:fail = $true; throw $m }
function Log($m) { Write-Host "[inject1] $m" }

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
if (-not (Test-Path $Raw))   { Fail "raw missing: $Raw" }
if (-not (Test-Path $Dist))  { Fail "dist missing: $Dist" }
$free = (Get-PSDrive -Name $Raw.Substring(0,1)).Free
Log ("free space on target drive: {0:N1} GB" -f ($free/1GB))

# peek MBR before
$fs = [IO.File]::OpenRead($Raw); $mbr = New-Object byte[] 512; [void]$fs.Read($mbr,0,512); $fs.Close()
Log ('MBR before: first16=' + (($mbr[0..15] | ForEach-Object { $_.ToString('x2') }) -join ' ') +
     ' active=' + $mbr[446].ToString('x2') + ' sig=' + $mbr[510].ToString('x2'))

# --- cleanup stale attach from a previous failed run -------------------------
# (VDS refuses to open/overwrite the vhd while it is attached; leftover letters
#  would also steal S:/T: from the letter pool below)
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

# --- convert raw -> vpc ------------------------------------------------------
# NOTE: diskpart/VDS refuses SPARSE vhd (error: "the virtual disk file ... must not be sparse").
# qemu-img dynamic vpc always has allocation holes -> convert to a temp file,
# then stream-copy (rewriting every byte makes NTFS allocate every cluster).
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

# --- pick free letters -------------------------------------------------------
$letters = $poolLetters |
    Where-Object { -not (Test-Path ("{0}:\" -f $_)) } | Select-Object -First 2
if ($letters.Count -lt 2) { Fail 'no free drive letters' }
$L1 = $letters[0]; $L2 = $letters[1]
Log ("letters: p1=$L1 p2=$L2")

# --- attach ------------------------------------------------------------------
Log 'attach vhd'
Run-DiskPart "attach vdisk`r`nselect partition 1`r`nassign letter=$L1`r`nselect partition 2`r`nassign letter=$L2"
if (-not (Test-Path "${L1}:\Windows\System32\winload.exe")) { Fail "attach/assign failed (S1 restore missing? L1=$L1)" }
Log 'attached; guards:'
if (-not (Test-Path "${L1}:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe")) { Fail 'guest powershell.exe missing' } else { Log '  powershell.exe OK' }
foreach ($f in 'bootmgr', 'Boot\BCD', 'Windows\System32\winload.exe') {
    Log ("  {0} = {1}" -f $f, (Test-Path "${L1}:\$f"))
}

# --- bcdboot (BIOS) ----------------------------------------------------------
Log 'bcdboot /f BIOS'
& "$env:SystemRoot\System32\bcdboot.exe" "${L1}:\Windows" /s "${L1}:" /f BIOS
if ($LASTEXITCODE -ne 0) { Fail "bcdboot rc=$LASTEXITCODE" }
foreach ($f in 'bootmgr', 'Boot\BCD') {
    if (-not (Test-Path "${L1}:\$f")) { Fail "bcdboot did not create \$f" }
}
Log 'bcdboot OK (bootmgr + \Boot\BCD)'

# --- bootsect MBR ------------------------------------------------------------
# correct syntax (probed live): bootsect /nt60 <letter>: /mbr
# (a bare `/mbr` before the target is rejected with usage -> rc=1)
$bs = "$env:SystemRoot\System32\bootsect.exe"
if (-not (Test-Path $bs)) { Fail "bootsect.exe missing on host: $bs" }
Log 'bootsect /nt60 + /mbr'
& $bs /nt60 "${L1}:" /mbr
$bsRc = $LASTEXITCODE
if ($bsRc -ne 0) {
    Log "bootsect rc=$bsRc, retry with /force"
    & $bs /nt60 "${L1}:" /mbr /force
    $bsRc = $LASTEXITCODE
}
if ($bsRc -ne 0) { Fail "bootsect failed rc=$bsRc (MBR code would stay blank -> S3 cannot boot)" }
Log 'bootsect OK'

# --- registry: autologon + RunOnce + UAC off --------------------------------
$AutoPw = 'zjtest'   # lab-only; lowercase so it can also be retyped via QEMU sendkey if ever needed
$reg = "$env:SystemRoot\System32\reg.exe"
$sp = "${L1}:\Windows\System32"
Log 'load SOFTWARE hive'
& $reg load HKLM\ZJSYS "$sp\config\SOFTWARE"
if ($LASTEXITCODE -ne 0) { Fail "reg load rc=$LASTEXITCODE" }
try {
    # NOTE (2026-10-17): NTUSER.DAT is Hidden -> Get-Item MUST get -Force, else
    # ItemNotFoundException; with $ErrorActionPreference='Stop' that terminated the
    # whole run mid-injection (raw left un-written, vhd left attached). The next
    # run self-heals via the stale-attach preflight. Profile timestamps are read
    # defensively (null on failure) so one odd profile cannot abort the injection.
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
        # no usable profile dir yet (pre-first-boot image: windeploy.exe creates the
        # profiles at next boot; ProfileList has only 18/19/20). unattend oobeSystem
        # AutoLogon Username=Administrator -> same fallback.
        $user = 'Administrator'
        Log 'no user profile found (pre-first-boot image) -> user=Administrator (unattend AutoLogon)'
    }

    $wl = 'HKLM\ZJSYS\Microsoft\Windows NT\CurrentVersion\Winlogon'
    & $reg add $wl /v AutoAdminLogon   /t REG_SZ /d 1          /f; if ($LASTEXITCODE) { Fail 'add AutoAdminLogon' }
    & $reg add $wl /v DefaultUserName  /t REG_SZ /d $user      /f; if ($LASTEXITCODE) { Fail 'add DefaultUserName' }
    # The account's REAL password is unknowable offline (unattend redacted by setup,
    # SAM hashes are SYSKEY-encrypted) -- so the GPO pre-logon script below sets it
    # to $AutoPw itself. Writing the same value here is belt-and-braces for a boot
    # where the startup script ends up running async (after Winlogon's first try).
    & $reg add $wl /v DefaultPassword  /t REG_SZ /d $AutoPw    /f; if ($LASTEXITCODE) { Fail 'add DefaultPassword' }
    & $reg add $wl /v DisableCAD       /t REG_SZ /d 1          /f; if ($LASTEXITCODE) { Fail 'add DisableCAD' }
    & $reg add 'HKLM\ZJSYS\Microsoft\Windows\CurrentVersion\RunOnce' /v zjautoa /t REG_SZ /d 'C:\zjgui\auto-a.cmd' /f
    if ($LASTEXITCODE) { Fail 'add RunOnce' }
    $psys = 'HKLM\ZJSYS\Microsoft\Windows\CurrentVersion\Policies\System'
    & $reg add $psys /v EnableLUA                  /t REG_DWORD /d 0 /f; if ($LASTEXITCODE) { Fail 'add EnableLUA' }
    & $reg add $psys /v ConsentPromptBehaviorAdmin /t REG_DWORD /d 0 /f; if ($LASTEXITCODE) { Fail 'add Consent' }
} finally {
    & $reg unload HKLM\ZJSYS
    if ($LASTEXITCODE) { Log "WARN: reg unload rc=$LASTEXITCODE" }
}
Log 'registry OK (autologon + RunOnce zjautoa + UAC off)'

# --- GPO pre-logon hook: set a KNOWN Administrator password ------------------
# Why: the lab image's unattend password was redacted by setup
# (*SENSITIVE*DATA*DELETED*) and SAM hashes are SYSKEY-encrypted, so the
# account's real password is unknowable from the offline image -> a plain
# autologon would stall on the logon screen and auto-a would never run.
#
# Fix = the same channel our product already uses for its first-boot EA applier
# (PIT-122/123/131; validated end-to-end in QEMU and on a real Win10): a LOCAL
# GROUP POLICY machine STARTUP script, i.e. code gpsvc runs as SYSTEM BEFORE the
# logon UI. Registry.pol carries RunStartupScriptSync + SyncForegroundPolicy so
# the script runs synchronously, pre-logon. It sets Administrator's password to
# $AutoPw (SYSTEM may change any local password via `net user`) and rewrites the
# autologon values, so the SAME boot then logs on unattended and consumes
# RunOnce -> auto-a. gpt.ini's Version uses a time-based low word so it cannot
# collide with a leftover State version (PIT-131: only the low 16 bits are
# compared; a collision makes gpsvc silently skip the whole policy pass).
$gpRoot  = "${L1}:\Windows\System32\GroupPolicy"
$gpMach  = "$gpRoot\Machine"
$gpStart = "$gpMach\Scripts\Startup"
New-Item -ItemType Directory -Path $gpStart -Force | Out-Null

$startupCmd = "$gpStart\zjlogon.cmd"
$cmdText = @'
@echo off
rem zj-e2e pre-logon hook: set a known Administrator password + autologon.
rem Runs as SYSTEM via the Machine\Scripts\Startup local GPO (see inject1.ps1).
set LOG=%SystemDrive%\zjgui\zjlogon.log
echo [%DATE% %TIME%] zjlogon start > "%LOG%"
net user Administrator @PW@ >> "%LOG%" 2>&1
net user Administrator /active:yes >> "%LOG%" 2>&1
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v AutoAdminLogon /t REG_SZ /d 1 /f >> "%LOG%" 2>&1
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v DefaultUserName /t REG_SZ /d Administrator /f >> "%LOG%" 2>&1
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v DefaultDomainName /t REG_SZ /d %COMPUTERNAME% /f >> "%LOG%" 2>&1
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v DefaultPassword /t REG_SZ /d @PW@ /f >> "%LOG%" 2>&1
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v DisableCAD /t REG_SZ /d 1 /f >> "%LOG%" 2>&1
echo [%DATE% %TIME%] zjlogon done >> "%LOG%"
exit /b 0
'@
$cmdText = ($cmdText.Replace('@PW@', $AutoPw)) -replace "`r?`n", "`r`n"
[IO.File]::WriteAllText($startupCmd, $cmdText, [Text.Encoding]::ASCII)

# scripts.ini: [Startup] entry (merge-safe / idempotent)
$iniPath = "$gpMach\Scripts\scripts.ini"
$iniTxt  = ''
if (Test-Path $iniPath) { $iniTxt = ([IO.File]::ReadAllText($iniPath)) -replace "`r", '' }
if ($iniTxt -notmatch 'zjlogon\.cmd') {
    $idx = 0
    if ($iniTxt.Trim()) {
        $nums = @([regex]::Matches($iniTxt, '(?m)^([0-9]+)') | ForEach-Object { [int]$_.Groups[1].Value })
        if ($nums.Count -gt 0) { $idx = ([int[]]$nums | Measure-Object -Maximum).Maximum + 1 }
        $lines = @($iniTxt -split "`n")
        $ins   = @("$idx`cmdLine=zjlogon.cmd", "$idx`Parameters=")
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
        $lines = @('[Startup]', "$idx`cmdLine=zjlogon.cmd", "$idx`Parameters=")
    }
    [IO.File]::WriteAllText($iniPath, (($lines -join "`r`n") + "`r`n"), [Text.Encoding]::ASCII)
    Log "gpo: scripts.ini written (idx=$idx)"
} else {
    Log 'gpo: scripts.ini already has zjlogon.cmd (left as-is)'
}

# gpt.ini: declare the Scripts + Registry CSE pairs and bump Version
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

# Registry.pol: our two sync records. Existing pol -> keep the file, append only
# our records (strip the fragment's 8-byte "PReg" header; records concatenate).
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

# verify (offline, before detach)
foreach ($f in $startupCmd, $iniPath, $gptPath, $polPath) {
    if (-not (Test-Path $f)) { Fail "GPO artifact missing: $f" }
    if ((Get-Item -LiteralPath $f -Force).Length -le 0) { Fail "GPO artifact empty: $f" }
}
$pb = [IO.File]::ReadAllBytes($polPath)
if ($pb.Length -lt 8 -or [Text.Encoding]::ASCII.GetString($pb, 0, 4) -ne 'PReg') { Fail 'Registry.pol magic wrong' }
$ptxt = [Text.Encoding]::Unicode.GetString($pb)
if ($ptxt -notmatch 'RunStartupScriptSync')  { Fail 'Registry.pol missing RunStartupScriptSync' }
if ($ptxt -notmatch 'SyncForegroundPolicy')  { Fail 'Registry.pol missing SyncForegroundPolicy' }
if (([IO.File]::ReadAllText($startupCmd)) -notmatch 'net user Administrator') { Fail 'zjlogon.cmd content wrong' }
Log ("gpt.ini = " + (([IO.File]::ReadAllText($gptPath)) -replace "`r`n", ' | ').Trim())
Log ("scripts.ini = " + (([IO.File]::ReadAllText($iniPath)) -replace "`r`n", ' | ').Trim())
Log "GPO pre-logon hook OK (SYSTEM sets Administrator pw=$AutoPw + autologon before logon UI)"

# --- copy package + automation ----------------------------------------------
Log ("robocopy dist -> ${L1}:\zjgui")
& robocopy $Dist "${L1}:\zjgui" /E /XD logs /R:1 /W:1 /NFL /NDL /NJH /NP | Out-Null
if ($LASTEXITCODE -ge 8) { Fail "robocopy rc=$LASTEXITCODE" }
Copy-Item (Join-Path $here 'auto-a.cmd')  "${L1}:\zjgui\" -Force
Copy-Item (Join-Path $here 'auto-a.ps1')  "${L1}:\zjgui\" -Force
foreach ($f in 'SysRecoverUI.exe', 'x64\SysRecoverUI.exe', 'auto-a.cmd', 'auto-a.ps1') {
    if (-not (Test-Path "${L1}:\zjgui\$f")) { Fail "missing zjgui\$f" }
}
Log 'zjgui package OK'

# --- marker ------------------------------------------------------------------
Set-Content -LiteralPath "${L1}:\Users\Public\Desktop\MARK-A.txt" -Value 'zj-e2e marker A' -Encoding ASCII
if (-not (Test-Path "${L1}:\Users\Public\Desktop\MARK-A.txt")) { Fail 'MARK-A write failed' }
Log 'MARK-A written'

# --- detach + convert back ---------------------------------------------------
if ($KeepAttached) { Log 'KeepAttached: leaving disks mounted'; exit 0 }
Log 'detach'
Run-DiskPart "select partition 1`r`nremove letter=$L1`r`nselect partition 2`r`nremove letter=$L2`r`ndetach vdisk"

Log 'convert vpc -> raw (tmp + swap)'
$tmpRaw = $Raw + '.tmp'
& $qimg convert -f vpc -O raw -S 64k $vhd $tmpRaw
if ($LASTEXITCODE -ne 0) { Fail 'qemu-img convert vpc->raw failed' }
Move-Item -LiteralPath $tmpRaw -Destination $Raw -Force

# --- verify MBR after round-trip --------------------------------------------
$fs = [IO.File]::OpenRead($Raw); $mbr = New-Object byte[] 512; [void]$fs.Read($mbr,0,512); $fs.Close()
Log ('MBR after : first16=' + (($mbr[0..15] | ForEach-Object { $_.ToString('x2') }) -join ' ') +
     ' active=' + $mbr[446].ToString('x2') + ' sig=' + $mbr[510].ToString('x2'))
if ($mbr[510] -ne 0x55 -or $mbr[511] -ne 0xAA) { Fail 'MBR signature lost in round-trip' }
if ($mbr[446] -ne 0x80) { Fail 'p1 active flag lost' }
$code = $false
for ($i = 0; $i -lt 4; $i++) { if ($mbr[$i] -ne 0) { $code = $true } }
if (-not $code) { Fail 'MBR boot code still blank after bootsect!' }
Log 'round-trip verify OK (boot code present, table intact)'
Remove-Item -LiteralPath $vhd -Force -ErrorAction SilentlyContinue   # ~21GB, regenerated each run

Remove-Item -LiteralPath $dpTmp -Force -ErrorAction SilentlyContinue
Log 'DONE - S1.5 injection complete'
exit 0
