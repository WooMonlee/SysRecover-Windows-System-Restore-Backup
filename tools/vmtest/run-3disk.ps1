# run-3disk.ps1 - rescue drill on the 3-disk replica of the 2026-10-04 customer.
#
# Layout (QEMU): ST1000=sda (D sda1 / F sda5 / G sda6), MA=sdb (C sdb1 /
# H sdb2 / I sdb3), KIOXIA=nvme0n1 (ESP nvme0n1p1 / MSR p2 / target p3).
# The task contract lives on the target root (nvme0n1p3); image + software dir
# live on F: (sda5). Boot: OVMF -> UEFI shell -> startup.nsh on the KIOXIA ESP
# -> rescue kernel -> scan -> offset-verified target -> mkntfs -> apply ->
# ESP sub-image -> RESTORE DONE. With 0.6.24 the black box (ZJRESTORE-last.log
# / -probe.txt / -status.txt) is also written to ESP + software logs dir.
#
# KEEP THIS FILE PURE ASCII (AGENTS.md section 15).
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\vmtest\run-3disk.ps1 [-Secs 360]

param([int]$Secs = 360, [int]$Mem = 2048, [int]$Smp = 2)

$ErrorActionPreference = "Continue"
$qemu = "D:\Prog\ProgIDE\msys64\mingw64\bin\qemu-system-x86_64.exe"
$ovmf = "D:\Prog\ProgIDE\msys64\mingw64\share\qemu\edk2-x86_64-code.fd"
$wd = Split-Path -Parent $MyInvocation.MyCommand.Path
$b = Join-Path $wd "base\3disk"
$ma = Join-Path $b "disk0-ma.vhd"
$st = Join-Path $b "disk1-st1000.vhd"
$kx = Join-Path $b "disk2-kioxia.vhd"
$log = Join-Path $wd ("logs\3disk-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + ".log")

if (!(Test-Path $qemu)) { Write-Host "qemu not found: $qemu"; exit 1 }
if (!(Test-Path $ovmf)) { Write-Host "OVMF not found: $ovmf"; exit 1 }
foreach ($f in @($ma, $st, $kx)) {
    if (!(Test-Path $f)) { Write-Host "missing $f (run mk-3disk-layout.py)"; exit 1 }
}

Write-Output "qemu: $qemu"
Write-Output "disks: $ma | $st | $kx"
Write-Output "log : $log"

$p = Start-Process -FilePath $qemu -WorkingDirectory $wd -ArgumentList @(
    "-accel", "tcg,thread=multi", "-cpu", "max", "-m", "$Mem", "-smp", "$Smp",
    "-machine", "q35", "-net", "none",
    "-drive", "if=pflash,format=raw,unit=0,readonly=on,file=$ovmf",
    "-drive", "id=hd0,file=$st,format=vpc,if=none",
    "-device", "ide-hd,drive=hd0,bus=ide.0,model=ST1000DM010-2EP102,serial=Z4FNRQMN",
    "-drive", "id=hd1,file=$ma,format=vpc,if=none",
    "-device", 'ide-hd,drive=hd1,bus=ide.1,model="MA 0902 SSD",serial=MA0902ZJ0001',
    "-drive", "id=nv0,file=$kx,format=vpc,if=none",
    "-device", "nvme,drive=nv0,serial=8CE38E03006EF5DE",
    "-nographic", "-no-reboot"
) -RedirectStandardOutput $log -RedirectStandardError ($log + ".err") -PassThru -NoNewWindow

$deadline = (Get-Date).AddSeconds($Secs)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 5
    if ($p.HasExited) { break }
    $t = Get-Content $log -Raw -ErrorAction SilentlyContinue
    if ($t -match "entering rescue shell" -or $t -match "RESTORE DONE") {
        Start-Sleep -Seconds 2
        break
    }
}
if (-not $p.HasExited) { Write-Output "stopping qemu"; $p.Kill() } else { Write-Output "qemu exited code=$($p.ExitCode)" }

$txt = Get-Content $log -Raw -ErrorAction SilentlyContinue
$checks = [ordered]@{
    "log found on nvme0n1p3"   = "found log: /tmp/zj_m/_zjresy041404.log (dev=/dev/nvme0n1p3"
    "target offset verified"   = "target from log location: /dev/nvme0n1p3 (offset verified)"
    "image found on F:"        = "found image: /tmp/zj_"
    "apply rc=0"               = "apply rc=0"
    "RESTORE DONE"             = "RESTORE DONE: /dev/nvme0n1p3"
}
$fail = 0
foreach ($k in $checks.Keys) {
    $ok = $txt -match [regex]::Escape($checks[$k])
    Write-Output ("  {0,-26} {1}" -f $k, $(if ($ok) { "PASS" } else { "FAIL" }))
    if (-not $ok) { $fail++ }
}
Write-Output ("  {0,-26} {1}" -f "esp subimage", $(if ($txt -match "esp restored \(rc=0\)") { "restored" } else { "skipped (no esp_index)" }))
Write-Output "== SR/ZJ lines (tail) =="
Select-String -Path $log -Pattern "SR:|ZJ:" -ErrorAction SilentlyContinue |
    ForEach-Object { $_.Line } | Select-Object -Last 45
if ($fail -eq 0) { Write-Output "3DISK: PASS"; exit 0 }
Write-Output "3DISK: FAIL ($fail checks)"; exit 1
