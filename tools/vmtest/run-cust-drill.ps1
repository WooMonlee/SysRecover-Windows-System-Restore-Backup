# run-cust-drill.ps1 - restore drill on the CUSTOMER-LAYOUT disk (base/custdrill.vhd).
#
# Proves: our Linux rescue + restore pipeline works on a byte-faithful replica of
# the faulty customer's partition structure (GPT: ESP 1GiB @LBA40 + MSR 16MiB +
# C: @LBA2129960, C: tail == backup GPT start; second data disk D/E/F/G).
#
# Boot: OVMF -> UEFI shell -> startup.nsh on the replica ESP -> kernel(EFI stub)
# -> rescue /init scans partitions -> finds restore-task.conf on the ESP ->
# locates target by offset (1090539520) -> mkntfs /dev/sda3 -> block apply of
# test.wim -> ESP sub-image restore -> RESTORE DONE.
#
# KEEP THIS FILE PURE ASCII (AGENTS.md section 15).
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\vmtest\run-cust-drill.ps1 [-Secs 360]

param([int]$Secs = 360)

$ErrorActionPreference = "Continue"
$qemu = "D:\Prog\ProgIDE\msys64\mingw64\bin\qemu-system-x86_64.exe"
$ovmf = "D:\Prog\ProgIDE\msys64\mingw64\share\qemu\edk2-x86_64-code.fd"
$wd = Split-Path -Parent $MyInvocation.MyCommand.Path
$vhd = Join-Path $wd "base\custdrill.vhd"
$log = Join-Path $wd ("logs\cust-drill-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + ".log")

if (!(Test-Path $qemu)) { Write-Host "qemu not found: $qemu"; exit 1 }
if (!(Test-Path $ovmf)) { Write-Host "OVMF not found: $ovmf"; exit 1 }
if (!(Test-Path $vhd)) { Write-Host "missing $vhd (run mk-customer-layout.py)"; exit 1 }

Write-Output "qemu: $qemu"
Write-Output "disk: $vhd"
Write-Output "log : $log"

$p = Start-Process -FilePath $qemu -WorkingDirectory $wd -ArgumentList @(
    "-accel", "tcg,thread=multi", "-cpu", "max", "-m", "2048", "-smp", "2",
    "-drive", "if=pflash,format=raw,unit=0,readonly=on,file=$ovmf",
    "-drive", "file=$vhd,format=vpc,if=ide,index=0",
    "-nographic", "-no-reboot"
) -RedirectStandardOutput $log -RedirectStandardError ($log + ".err") -PassThru -NoNewWindow

$p | Wait-Process -Timeout $Secs -ErrorAction SilentlyContinue
if (-not $p.HasExited) { Write-Output "timeout -> killing"; $p.Kill() } else { Write-Output "qemu exited code=$($p.ExitCode)" }

$txt = Get-Content $log -Raw -ErrorAction SilentlyContinue
$checks = [ordered]@{
    "log found on sda3"        = "found log: /tmp/zj_m/_zjresy-cust.log (dev=/dev/sda3)"
    "target from log location" = "target from log location: /dev/sda3"
    "mkntfs start_lba=2129960" = "mkntfs /dev/sda3 (start_lba=2129960)"
    "hidden sectors OK"        = "hidden sectors = 2129960 (OK)"
    "apply succeeded"          = "apply rc=0"
    "RESTORE DONE on sda3"     = "RESTORE DONE: /dev/sda3"
}
$fail = 0
foreach ($k in $checks.Keys) {
    $ok = $txt -match [regex]::Escape($checks[$k])
    Write-Output ("  {0,-26} {1}" -f $k, $(if ($ok) { "PASS" } else { "FAIL" }))
    if (-not $ok) { $fail++ }
}
Write-Output "== SR/ZJ lines (tail) =="
Select-String -Path $log -Pattern "SR:|ZJ:" -ErrorAction SilentlyContinue |
    ForEach-Object { $_.Line } | Select-Object -Last 40
if ($fail -eq 0) { Write-Output "CUST-DRILL: PASS"; exit 0 }
Write-Output "CUST-DRILL: FAIL ($fail checks)"; exit 1
