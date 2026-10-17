# inspect3.ps1 - read-only assertion legs for the S3/S3b/S4/S5 stages (ASCII, PS5.1)
#   -Leg s3  : after the S3 leg the guest staged an A-engine swap: C:\~new holds system B,
#              C:\.zj-keepold marks keep-old, C:\zjgui\restore-task.conf carries the
#              contract, the old system A is still live at C:\ root.
#   -Leg s3b : after the rescue swap: root = system B, old system A moved to C:\~old.
#   -Leg s4  : after injection into system B (now at root): MARK-B + RunOnce + autologon.
#   -Leg s5  : after the first boot of B chose "delete old system": C:\~old is gone.
# raw -> vhd, attach, assert, detach (never converts back; raw is authoritative).
param([string]$Raw = '', [string]$Leg = 's3')
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Raw) { $Raw = (Resolve-Path (Join-Path $here '..\base\dual.raw')).Path }
$vhd = [IO.Path]::ChangeExtension($Raw, '.vhd')
$qimg = 'D:\Prog\ProgIDE\msys64\mingw64\bin\qemu-img.exe'
$dpTmp = Join-Path $env:TEMP ('zjdp3-' + (Get-Random) + '.txt')
$poolLetters = @('S','T','U','V','W','X','Y','Z','P','Q','R','N','M')
$script:vhd = $vhd
$script:dpTmp = $dpTmp
$script:fail = 0
function Log($m) { Write-Output "[inspect3] $m" }
function Run-DiskPart([string]$body) {
    $content = "select vdisk file=`"$script:vhd`"`r`n" + $body + "`r`nexit`r`n"
    Set-Content -LiteralPath $script:dpTmp -Value $content -Encoding ASCII
    $out = & diskpart /s $script:dpTmp 2>&1 | Out-String
    Log ("diskpart rc=$LASTEXITCODE :: " + ($out -replace '\s+', ' ').Trim())
}
function Check($name, $cond, $extra) {
    if ($cond) { Log ("OK   $name" + $(if ($extra) { " :: $extra" } else { '' })) }
    else { $script:fail++; Log ("FAIL $name" + $(if ($extra) { " :: $extra" } else { '' })) }
}
function Head($p, $n, $label) {
    if (Test-Path -LiteralPath $p) {
        Log ("--- $label ---")
        Get-Content -LiteralPath $p -TotalCount $n -ErrorAction SilentlyContinue | ForEach-Object { Write-Output $_ }
    } else { Log ("MISSING $label") }
}

if (Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue) { Log 'QEMU RUNNING - abort'; exit 2 }

$stale = @()
foreach ($l in $poolLetters) {
    if (-not (Test-Path "${l}:\")) { continue }
    $pt = Get-Partition -DriveLetter $l -ErrorAction SilentlyContinue
    if (-not $pt) { continue }
    $dk = $pt | Get-Disk
    if ($dk.BusType -eq 'File Backed Virtual') { $stale += $l }
}
if ($stale.Count -gt 0) {
    Log ("stale attach: " + ($stale -join ','))
    $body = ''
    foreach ($l in $stale) { $body += "select volume ${l}`r`nremove letter=${l}`r`n" }
    $body += 'detach vdisk'
    Run-DiskPart $body
}

Log 'convert raw -> vpc + de-sparsify'
$part = $vhd + '.part'
& $qimg convert -f raw -O vpc $Raw $part
if ($LASTEXITCODE -ne 0) { Log 'convert failed'; exit 3 }
$s = [IO.File]::OpenRead($part); $d = [IO.File]::Create($vhd)
$buf = New-Object byte[] (64MB)
try { while (($n = $s.Read($buf, 0, $buf.Length)) -gt 0) { $d.Write($buf, 0, $n) } }
finally { $d.Close(); $s.Close() }
Remove-Item -LiteralPath $part -Force

$letters = $poolLetters | Where-Object { -not (Test-Path ("{0}:\" -f $_)) } | Select-Object -First 2
if ($letters.Count -lt 2) { Log 'no free letters'; exit 3 }
$L1 = $letters[0]; $L2 = $letters[1]
Log ("letters: p1=$L1 p2=$L2")
Run-DiskPart "attach vdisk`r`nselect partition 1`r`nassign letter=$L1`r`nselect partition 2`r`nassign letter=$L2"

$c = "${L1}:"
Log ("leg=$Leg")
# ---- assertions per leg ----------------------------------------------------
if ($Leg -eq 's3') {
    Check 'old system A still at root (winload)' (Test-Path "$c\Windows\System32\winload.exe")
    Check 'old system A mark (MARK-A.txt)' (Test-Path "$c\Users\Public\Desktop\MARK-A.txt")
    Check 'new system B extracted (C:\~new\.zj-done)' (Test-Path "$c\~new\.zj-done")
    Check 'new system B winload' (Test-Path "$c\~new\Windows\System32\winload.exe")
    Check 'keepold marker C:\.zj-keepold' (Test-Path "$c\.zj-keepold")
    Check 'no C:\~old yet' (-not (Test-Path "$c\~old"))
    Check 'program deployed into ~new' (Test-Path "$c\~new\Program Files\SysRecover\SysRecoverUI.exe")
    Check 'shortcut in ~new public desktop' (Test-Path "$c\~new\Users\Public\Desktop\SysRecover.lnk")
    Check 'contract in exe dir (C:\zjgui\restore-task.conf)' (Test-Path "$c\zjgui\restore-task.conf")
    Check 'rescue launcher at C:\grldr' (Test-Path "$c\grldr")
    Check 'rescue launcher at C:\grldr.mbr' (Test-Path "$c\grldr.mbr")
    Check 'rescue menu at C:\menu.lst' (Test-Path "$c\menu.lst")
    Check 'rescue payload C:\ZJRESTORE\boot' (Test-Path "$c\ZJRESTORE\boot")
    $zl = @(Get-ChildItem "$c" -Filter '_zjresy*.log' -File -Force -ErrorAction SilentlyContinue)
    Check 'target-root log _zjresy*.log' ($zl.Count -gt 0) (($zl | Select-Object -ExpandProperty Name) -join ',')
    foreach ($f in 'grldr','grldr.mbr','menu.lst') {
        $it = Get-Item -LiteralPath "$c\$f" -Force -ErrorAction SilentlyContinue
        if ($it) { Check "attr $f" $true ("0x{0:x}" -f [int]$it.Attributes) } else { Check "attr $f" $false }
    }
    $kb = Get-Item -LiteralPath "$c\.zj-keepold" -Force -ErrorAction SilentlyContinue
    if ($kb) { Check 'zj-keepold exists' $true ("len=" + $kb.Length) }
    Head "$c\zjgui\restore-task.conf" 40 'restore-task.conf'
    if ($zl.Count -gt 0) { Head $zl[0].FullName 40 'target-root _zjresy log' }
}
elseif ($Leg -eq 's3b') {
    Check 'old system A moved to C:\~old' (Test-Path "$c\~old\Windows\System32\winload.exe")
    Check 'old system A mark inside ~old' (Test-Path "$c\~old\Users\Public\Desktop\MARK-A.txt")
    Check 'no MARK-A at root anymore' (-not (Test-Path "$c\Users\Public\Desktop\MARK-A.txt"))
    Check 'new system B at root (winload)' (Test-Path "$c\Windows\System32\winload.exe")
    Check 'C:\~new gone' (-not (Test-Path "$c\~new"))
    Check 'keepold marker inside ~old (swapleft.cpp:354)' (Test-Path "$c\~old\.zj-keepold")
    Check 'bootmgr at root' (Test-Path "$c\bootmgr")
    Check 'BCD at root (\Boot\BCD)' (Test-Path "$c\Boot\BCD")
    Check 'system B program deployed at root' (Test-Path "$c\Program Files\SysRecover\SysRecoverUI.exe")
    Log ('root listing: ' + ((Get-ChildItem "$c" -Force -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name) -join ', '))
}
elseif ($Leg -eq 's4') {
    Check 'system B at root' (Test-Path "$c\Windows\System32\winload.exe")
    Check 'MARK-B injected' (Test-Path "$c\Users\Public\Desktop\MARK-B.txt")
    Check 'old system A still at ~old' (Test-Path "$c\~old\Users\Public\Desktop\MARK-A.txt")
    Check 'keepold marker still in ~old' (Test-Path "$c\~old\.zj-keepold")
    Check 'transient restore-task.conf cleared' (-not (Test-Path "$c\restore-task.conf"))
    Check 'auto-b script injected' (Test-Path "$c\zjgui\auto-b.ps1")
    Check 'auto-b cmd injected' (Test-Path "$c\zjgui\auto-b.cmd")
    Check 'GPO startup script zjlogonb.cmd' (Test-Path "$c\Windows\System32\GroupPolicy\Machine\Scripts\Startup\zjlogonb.cmd")
    Check 'installed program skin' (Test-Path "$c\Program Files\SysRecover\skin\main.xml")
    Check 'installed program lang' (Test-Path "$c\Program Files\SysRecover\lang\en.lang")
    $gl = @(Get-ChildItem "$c\zjgui" -Filter 'auto-b.log' -File -Force -ErrorAction SilentlyContinue)
    Check 'no auto-b log yet' ($gl.Count -eq 0)
    $reg = "$env:SystemRoot\System32\reg.exe"
    & $reg load HKLM\ZJSYS "${c}\Windows\System32\config\SOFTWARE" 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) {
        $q = (& $reg query 'HKLM\ZJSYS\Microsoft\Windows\CurrentVersion\RunOnce' 2>&1 | Out-String)
        Log ('RunOnce: ' + ($q -replace '\s+', ' ').Trim())
        Check 'RunOnce zjautob' ($q -match 'auto-b')
        $w = (& $reg query 'HKLM\ZJSYS\Microsoft\Windows NT\CurrentVersion\Winlogon' /v AutoAdminLogon 2>&1 | Out-String)
        Check 'AutoAdminLogon=1' ($w -match '0x1')
        $u = (& $reg query 'HKLM\ZJSYS\Microsoft\Windows NT\CurrentVersion\Winlogon' /v DefaultUserName 2>&1 | Out-String)
        Log ('DefaultUserName: ' + ($u -replace '\s+', ' ').Trim())
        $lua = (& $reg query 'HKLM\ZJSYS\Microsoft\Windows\CurrentVersion\Policies\System' /v EnableLUA 2>&1 | Out-String)
        Check 'EnableLUA=0' ($lua -match '0x0')
        & $reg unload HKLM\ZJSYS 2>&1 | Out-Null
    } else { Check 'SOFTWARE hive load' $false }
}
elseif ($Leg -eq 's5') {
    Check 'C:\~old removed (delete-old-system)' (-not (Test-Path "$c\~old"))
    Check 'keepold marker removed' (-not (Test-Path "$c\.zj-keepold"))
    Check 'system B still at root' (Test-Path "$c\Windows\System32\winload.exe")
    Check 'MARK-B still present' (Test-Path "$c\Users\Public\Desktop\MARK-B.txt")
    $gl = @(Get-ChildItem "$c\zjgui" -Filter 'auto-b.log' -File -Force -ErrorAction SilentlyContinue)
    if ($gl.Count -gt 0) { Head $gl[0].FullName 60 'auto-b.log' } else { Log 'MISSING C:\zjgui\auto-b.log' }
}
else { Log "unknown leg: $Leg"; $script:fail++ }

# ---- detach + cleanup ------------------------------------------------------
Log 'detach'
Run-DiskPart "select partition 1`r`nremove letter=$L1`r`nselect partition 2`r`nremove letter=$L2`r`ndetach vdisk"
Remove-Item -LiteralPath $vhd -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $dpTmp -Force -ErrorAction SilentlyContinue
Log ("DONE fails=$script:fail")
if ($script:fail -gt 0) { exit 1 }
exit 0
