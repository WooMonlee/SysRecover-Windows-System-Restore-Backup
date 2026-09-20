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
Write-Output "== SR/ZJ lines =="
Select-String -Path $log -Pattern "SR:|ZJ:|Kernel panic|reboot:" | ForEach-Object { $_.Line } | Select-Object -Last 45
