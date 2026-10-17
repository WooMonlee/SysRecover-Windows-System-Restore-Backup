# inspect1.ps1 - read-only probe of dual.raw after an S3/S5 leg (ASCII only, PS5.1)
# convert raw -> vhd (de-sparsified), diskpart attach, dump key evidence, detach,
# delete the vhd.  NEVER converts back (raw is authoritative; guest wrote it).
param([string]$Raw = '', [int]$TailLines = 25, [string]$Leg = 's3')
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Raw) { $Raw = (Resolve-Path (Join-Path $here '..\base\dual.raw')).Path }
$vhd = [IO.Path]::ChangeExtension($Raw, '.vhd')
$qimg = 'D:\Prog\ProgIDE\msys64\mingw64\bin\qemu-img.exe'
$dpTmp = Join-Path $env:TEMP ('zjdp-' + (Get-Random) + '.txt')
$poolLetters = @('S','T','U','V','W','X','Y','Z','P','Q','R','N','M')
$script:vhd = $vhd
$script:dpTmp = $dpTmp
function Log($m) { Write-Output "[inspect1] $m" }
function Run-DiskPart([string]$body) {
    $content = "select vdisk file=`"$script:vhd`"`r`n" + $body + "`r`nexit`r`n"
    Set-Content -LiteralPath $script:dpTmp -Value $content -Encoding ASCII
    $out = & diskpart /s $script:dpTmp 2>&1 | Out-String
    Log ("diskpart rc=$LASTEXITCODE :: " + ($out -replace '\s+', ' ').Trim())
}
function Tail($p, $n) {
    if (Test-Path -LiteralPath $p) {
        Log ("--- {0} (tail {1}) ---" -f $p, $n)
        Get-Content -LiteralPath $p -Tail $n -ErrorAction SilentlyContinue | ForEach-Object { Write-Output $_ }
    } else { Log ("MISSING {0}" -f $p) }
}
function GrepCount($p, $pat) {
    if (Test-Path -LiteralPath $p) {
        $c = @(Select-String -LiteralPath $p -Pattern $pat -ErrorAction SilentlyContinue)
        Log ("grep '{0}' in {1}: {2} hit(s)" -f $pat, (Split-Path -Leaf $p), $c.Count)
    }
}

if (Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue) { Log 'QEMU RUNNING - abort'; exit 2 }

# stale attach cleanup
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

# ---- evidence --------------------------------------------------------------
Tail "${L1}:\Windows\Panther\setupact.log" $TailLines
Tail "${L1}:\Windows\Panther\setuperr.log" $TailLines
Tail "${L1}:\Windows\Panther\unattendgc\setupact.log" $TailLines
Tail "${L1}:\Windows\Panther\unattendgc\setuperr.log" $TailLines
Tail "${L1}:\Windows\Setup\Scripts\SetupComplete.cmd" 30
Tail "${L1}:\zjgui\auto-a.log" 40
Tail "${L1}:\zjgui\zjlogon.log" 20
Tail "${L1}:\Users\Public\Desktop\MARK-A.txt" 3
$gl = @(Get-ChildItem "${L2}:\ZJRESTORE\logs" -Filter 'SysRecover-*.log' -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending)
if ($gl.Count -gt 0) { Tail $gl[0].FullName 45 } else { Log ("MISSING " + $L2 + ":\ZJRESTORE\logs\SysRecover-*.log") }
Tail "${L2}:\auto-a.log" 40
Log ($L2 + ":\ZJRESTORE\logs = " + ((Get-ChildItem "${L2}:\ZJRESTORE\logs" -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name) -join ', '))

Log ("Users: " + ((Get-ChildItem "${L1}:\Users" -Directory -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name) -join ', '))
Log ("sysprep dir: " + (Test-Path "${L1}:\Windows\System32\Sysprep\sysprep.exe"))
Log ("Panther files: " + ((Get-ChildItem "${L1}:\Windows\Panther" -File -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name) -join ', '))
Log ("Panther unattendgc: " + ((Get-ChildItem "${L1}:\Windows\Panther\unattendgc" -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name) -join ', '))

$reg = "$env:SystemRoot\System32\reg.exe"
& $reg load HKLM\ZJSYS "${L1}:\Windows\System32\config\SOFTWARE" 2>&1 | Out-Null
if ($LASTEXITCODE -eq 0) {
    foreach ($k in 'HKLM\ZJSYS\Microsoft\Windows\CurrentVersion\RunOnce',
                   'HKLM\ZJSYS\Microsoft\Windows NT\CurrentVersion\Winlogon',
                   'HKLM\ZJSYS\Microsoft\Windows\CurrentVersion\Policies\System') {
        Log ("reg query $k")
        (& $reg query $k 2>&1) | ForEach-Object { Write-Output $_ }
    }
    & $reg unload HKLM\ZJSYS 2>&1 | Out-Null
} else { Log 'SOFTWARE load failed' }

& $reg load HKLM\ZJSYS2 "${L1}:\Windows\System32\config\SYSTEM" 2>&1 | Out-Null
if ($LASTEXITCODE -eq 0) {
    Log 'reg query HKLM\ZJSYS2\Setup'
    (& $reg query 'HKLM\ZJSYS2\Setup' 2>&1) | ForEach-Object { Write-Output $_ }
    & $reg unload HKLM\ZJSYS2 2>&1 | Out-Null
} else { Log 'SYSTEM load failed' }

if ($Leg -eq 's5') {
    Log ("zjgui listing: " + ((Get-ChildItem "${L1}:\zjgui" -Force -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name) -join ', '))
    Tail "${L1}:\zjgui\auto-b.log" 60
    Tail "${L1}:\zjgui\zjlogonb.log" 20
    Tail "${L1}:\zjgui\zjautoboot.log" 30
    Log ("claimed file: " + (Test-Path "${L1}:\zjgui\auto-b.claimed"))
    Log ("~old present: " + (Test-Path "${L1}:\~old"))
    Log ("root listing: " + ((Get-ChildItem "${L1}:\" -Force -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name) -join ', '))
    Log ("Administrator profile: " + (Test-Path "${L1}:\Users\Administrator"))
    Log ("Users: " + ((Get-ChildItem "${L1}:\Users" -Force -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name) -join ', '))
    Log ("gp startup dir: " + ((Get-ChildItem "${L1}:\Windows\System32\GroupPolicy\Machine\Scripts\Startup" -Force -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name) -join ', '))
    Tail "${L1}:\Windows\System32\GroupPolicy\Machine\Scripts\scripts.ini" 20
    Tail "${L1}:\Windows\System32\GroupPolicy\Machine\gpt.ini" 10
    Tail "${L1}:\Users\Public\Desktop\MARK-B.txt" 3
    Log ("pending-restore on p2: " + (Test-Path "${L2}:\pending-restore.txt"))
    Log ("p2 auto-b.log: " + (Test-Path "${L2}:\auto-b.log"))

    # ---- crash evidence (S5: app died inside swapleft takeown) ---------------
    Log ("~old\.zj-keepold: " + (Test-Path "${L1}:\~old\.zj-keepold"))
    Log ("~old\Windows: " + (Test-Path "${L1}:\~old\Windows"))
    Log ("~old top: " + ((Get-ChildItem "${L1}:\~old" -Force -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name) -join ', '))
    Log ("Minidump: " + ((Get-ChildItem "${L1}:\Windows\Minidump" -Force -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name) -join ', '))
    Log ("MEMORY.DMP: " + (Test-Path "${L1}:\Windows\MEMORY.DMP"))
    Log ("LiveKernelReports: " + ((Get-ChildItem "${L1}:\Windows\LiveKernelReports" -Force -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name) -join ', '))
    Log ("CrashDumps: " + ((Get-ChildItem "${L1}:\Users\Administrator\AppData\Local\CrashDumps" -Force -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name) -join ', '))
    Log ("WER queue: " + ((Get-ChildItem "${L1}:\ProgramData\Microsoft\Windows\WER\ReportQueue" -Force -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name) -join ', '))
    Log ("WER archive: " + ((Get-ChildItem "${L1}:\ProgramData\Microsoft\Windows\WER\ReportArchive" -Force -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name) -join ', '))
    Log ("crash dir p2: " + ((Get-ChildItem "${L2}:\ZJRESTORE\logs\crash" -Force -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name) -join ', '))
    Log ("collected p2 top: " + ((Get-ChildItem "${L2}:\ZJRESTORE\logs\collected" -Force -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name) -join ', '))
    if ($gl.Count -gt 0) {
        foreach ($gp in 'tree delete begin', 'takeown begin', 'takeown done', 'tree top', 'rec enter',
                         'message loop exited', 'delete done', 'delete incomplete', 'delete UI',
                         '[VEH]', 'CRASH') {
            GrepCount $gl[0].FullName $gp
        }
    }
}

if ($Leg -eq 's5b') {
    # clone-first-boot leg: product must auto-start (RunOnce), migrate logs into
    # C:\Program Files\SysRecover\logs, then the 3-choice dialog; zjclick deletes
    # ~old.  Logs live in the INSTALL dir now, not on p2.
    $appLogs = "${L1}:\Program Files\SysRecover\logs"
    Log ("app logs listing: " + ((Get-ChildItem $appLogs -Force -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name) -join ', '))
    Log ("app logs collected: " + ((Get-ChildItem "$appLogs\collected" -Force -Recurse -ErrorAction SilentlyContinue | Where-Object { -not $_.PSIsContainer } | Select-Object -ExpandProperty FullName) -join ', '))
    Log ("planted ZJRESTORE-last.log: " + (Test-Path "$appLogs\ZJRESTORE-last.log"))
    Log ("planted ZJRESTORE-status.txt: " + (Test-Path "$appLogs\ZJRESTORE-status.txt"))
    Log ("planted zj-demo-migrate.log: " + (Test-Path "$appLogs\zj-demo-migrate.log"))
    $al = @(Get-ChildItem $appLogs -Filter 'SysRecover-*.log' -File -Force -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending)
    if ($al.Count -gt 0) {
        Tail $al[0].FullName 40
        GrepCount $al[0].FullName 'log migrate'
        Log ("--- log migrate lines ---")
        (Select-String -LiteralPath $al[0].FullName -Pattern 'log migrate' -ErrorAction SilentlyContinue) |
            ForEach-Object { Write-Output ("  " + $_.Line) }
        GrepCount $al[0].FullName 'inventory:'
    } else { Log 'MISSING app log' }

    Log ("~old present: " + ((Get-ChildItem "${L1}:\~old" -Force -ErrorAction SilentlyContinue | Measure-Object).Count -gt 0) + " (marker kept: " + (Test-Path "${L1}:\~old\.zj-keepold") + ")")
    Log ("p2 ZJRESTORE dir: " + (Test-Path "${L2}:\ZJRESTORE"))
    Log ("p2 ZJRESTORE-logs (want False): " + (Test-Path "${L2}:\ZJRESTORE-logs"))
    $zi = Get-Item -LiteralPath "${L2}:\ZJRESTORE" -Force -ErrorAction SilentlyContinue
    if ($zi) { Log ("p2 ZJRESTORE attributes: " + $zi.Attributes + " mtime=" + $zi.LastWriteTime) } else { Log 'p2 ZJRESTORE missing' }
    Log 'p2 root attrib:'
    (& "$env:SystemRoot\System32\attrib.exe" "${L2}:\" 2>&1) | ForEach-Object { Write-Output ("  " + $_) }
    Log ("p2 ZJRESTORE top: " + ((Get-ChildItem "${L2}:\ZJRESTORE" -Force -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name) -join ', '))
    Tail "${L1}:\zjgui\auto-b.log" 20
    Log ("crash dir: " + ((Get-ChildItem "$appLogs\crash" -Force -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name) -join ', '))
    Log ("Minidump: " + ((Get-ChildItem "${L1}:\Windows\Minidump" -Force -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name) -join ', '))
}

# ---- detach + cleanup ------------------------------------------------------
Log 'detach'
Run-DiskPart "select partition 1`r`nremove letter=$L1`r`nselect partition 2`r`nremove letter=$L2`r`ndetach vdisk"
Remove-Item -LiteralPath $vhd -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $dpTmp -Force -ErrorAction SilentlyContinue
Log 'DONE'
exit 0
