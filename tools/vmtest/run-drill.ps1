# run-drill.ps1 - run one restore drill under QEMU (Alpine base).
#
# Why PowerShell instead of run-drill.sh: Git Bash kill cannot kill native
# qemu on Windows, so the shell script hangs. Start-Process/Wait-Process/Kill
# in PowerShell is reliable.
#
# Usage (admin PowerShell):
#   powershell -ExecutionPolicy Bypass -File tools\vmtest\run-drill.ps1 -Secs 160
#
# Success marker: log ends with "reboot: Restarting system" (with -no-reboot
# QEMU exits by itself).
#
# NOTE: keep this file ASCII-only. PowerShell 5.1 parses .ps1 as ANSI when
# there is no BOM, so CJK comments break the parser.
param([int]$Secs = 160)
$ErrorActionPreference = 'Continue'
$qemu = "D:\Prog\ProgIDE\msys64\mingw64\bin\qemu-system-x86_64.exe"
$wd = Split-Path -Parent $MyInvocation.MyCommand.Path
$log = Join-Path $wd ("logs\drill-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + ".log")
$p = Start-Process -FilePath $qemu -WorkingDirectory $wd -ArgumentList @(
    "-accel", "tcg,thread=multi", "-cpu", "max", "-m", "1024",
    "-drive", "file=base/drill.raw,format=raw,if=ide",
    "-boot", "c", "-no-reboot", "-nographic"
) -RedirectStandardOutput $log -RedirectStandardError ($log + ".err") -PassThru -NoNewWindow
Write-Output "qemu pid=$($p.Id) log=$log"
$p | Wait-Process -Timeout $Secs -ErrorAction SilentlyContinue
if (-not $p.HasExited) { Write-Output "timeout -> killing"; $p.Kill() } else { Write-Output "qemu exited code=$($p.ExitCode)" }
$txt = Get-Content $log -Raw -ErrorAction SilentlyContinue
$checks = [ordered]@{
    "found log + offset match"  = "offset match 105906176"
    "target from log (verified)"= "target from log location: /dev/sda2 (offset verified)"
    "mkntfs start_lba=206848"   = "mkntfs /dev/sda2 (start_lba=206848)"
    "apply rc=0"                = "apply rc=0"
    "ESP subimage restored"     = "esp restored (rc=0)"
    "blackbox to ESP"           = "blackbox: log -> /dev/sda1 (ESP)"
    "RESTORE DONE"              = "RESTORE DONE: /dev/sda2"
}
$fail = 0
foreach ($k in $checks.Keys) {
    $ok = $txt -match [regex]::Escape($checks[$k])
    Write-Output ("  {0,-26} {1}" -f $k, $(if ($ok) { "PASS" } else { "FAIL" }))
    if (-not $ok) { $fail++ }
}
Write-Output "== SR/ZJ lines (tail) =="
Select-String -Path $log -Pattern "SR:|ZJ:|Kernel panic|reboot:" | ForEach-Object { $_.Line } | Select-Object -Last 30
if ($fail -eq 0) { Write-Output "DRILL: PASS"; exit 0 }
Write-Output "DRILL: FAIL ($fail checks)"; exit 1
