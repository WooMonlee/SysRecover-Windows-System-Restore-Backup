# run-swap-setup.ps1 - First boot: format sda2 as NTFS and populate ~new + old files.
# Uses the same initramfs but with zjsetup=1 on the kernel command line.
# Usage: powershell -ExecutionPolicy Bypass -File graveyard\tools\vmtest\run-swap-setup.ps1
param([int]$Secs = 90)
$ErrorActionPreference = 'Continue'
$qemu = "D:\Prog\ProgIDE\msys64\mingw64\bin\qemu-system-x86_64.exe"
$wd = Split-Path -Parent $MyInvocation.MyCommand.Path
$log = Join-Path $wd ("logs\swap-setup-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + ".log")
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
    "setup mode entered"  = "setup mode: running setup-swap.sh"
    "mkntfs done"         = "mkntfs done"
    "mounted"             = "mounted via"
    "setup done"          = "setup done"
    "SETUP COMPLETE"      = "SETUP COMPLETE"
}
$fail = 0
foreach ($k in $checks.Keys) {
    $ok = $txt -match [regex]::Escape($checks[$k])
    Write-Output ("  {0,-26} {1}" -f $k, $(if ($ok) { "PASS" } else { "FAIL" }))
    if (-not $ok) { $fail++ }
}
Write-Output "== SETUP lines =="
Select-String -Path $log -Pattern "SETUP:|SETUP |Kernel panic|setup " | ForEach-Object { $_.Line } | Select-Object -Last 20
if ($fail -eq 0) { Write-Output "SETUP: PASS"; exit 0 }
Write-Output "SETUP: FAIL ($fail checks)"; exit 1
