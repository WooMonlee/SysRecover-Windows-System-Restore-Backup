# run-swap-drill.ps1 - run A-engine swap drill under QEMU.
#
# Usage (admin PowerShell):
#   powershell -ExecutionPolicy Bypass -File graveyard\tools\vmtest\run-swap-drill.ps1
#
# Success: "swap rc=0" + "RESTORE DONE" + "reboot: Restarting system"
param([int]$Secs = 120)
$ErrorActionPreference = 'Continue'
$qemu = "D:\Prog\ProgIDE\msys64\mingw64\bin\qemu-system-x86_64.exe"
$wd = Split-Path -Parent $MyInvocation.MyCommand.Path
$log = Join-Path $wd ("logs\swap-drill-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + ".log")
$p = Start-Process -FilePath $qemu -WorkingDirectory $wd -ArgumentList @(
    "-accel", "tcg,thread=multi", "-cpu", "max", "-m", "1024",
    "-drive", "file=base/swap-drill.raw,format=raw,if=ide",
    "-boot", "c", "-no-reboot", "-nographic"
) -RedirectStandardOutput $log -RedirectStandardError ($log + ".err") -PassThru -NoNewWindow
Write-Output "qemu pid=$($p.Id) log=$log"
$p | Wait-Process -Timeout $Secs -ErrorAction SilentlyContinue
if (-not $p.HasExited) { Write-Output "timeout -> killing"; $p.Kill() } else { Write-Output "qemu exited code=$($p.ExitCode)" }
$txt = Get-Content $log -Raw -ErrorAction SilentlyContinue
$checks = [ordered]@{
    "engine=swap read"          = "engine=swap"
    "swap started"              = "A-engine swap: ~new -> root"
    "swap rc=0"                 = "swap rc=0"
    "swap done"                 = "swap done: ~new -> root, old -> ~old"
    "RESTORE DONE"              = "RESTORE DONE: /dev/sda2"
    "reboot"                    = "reboot: Restarting system"
}
$fail = 0
foreach ($k in $checks.Keys) {
    $ok = $txt -match [regex]::Escape($checks[$k])
    Write-Output ("  {0,-26} {1}" -f $k, $(if ($ok) { "PASS" } else { "FAIL" }))
    if (-not $ok) { $fail++ }
}
Write-Output "== SR/ZJ/SWAP lines (tail) =="
Select-String -Path $log -Pattern "SR:|ZJ:|SWAP:|Kernel panic|reboot:|engine=" | ForEach-Object { $_.Line } | Select-Object -Last 40
if ($fail -eq 0) { Write-Output "SWAP DRILL: PASS"; exit 0 }
Write-Output "SWAP DRILL: FAIL ($fail checks)"; exit 1
