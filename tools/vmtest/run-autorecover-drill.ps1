# run-autorecover-drill.ps1 - P3 auto-recover drill (PIT-148).
#
# Build the disk first (block engine, AUTORECOVER contract):
#   $env:ZJ_DRILL_AUTORECOVER="1"
#   D:\Prog\ProgIDE\Python\Python313\python.exe tools\vmtest\mk-drill.py
# Then:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\vmtest\run-autorecover-drill.ps1
#
# The contract points at a nonexistent image, so the rescue layer fails at
# STEP=image-not-found with STAGE=0 (target untouched). Expected new behavior:
#   * trap writes "recover decision: reboot (target untouched, ...)"
#   * init prints "RESULT: restore FAILED" then AUTO-RECOVERs (reboot -f)
#   * with -no-reboot QEMU exits by itself
#   * NO bare shell: "entering rescue shell" and the recovery menu must NOT
#     appear on this path.
#
# NOTE: keep this file ASCII-only.
param([int]$Secs = 120)
$ErrorActionPreference = 'Continue'
$qemu = "D:\Prog\ProgIDE\msys64\mingw64\bin\qemu-system-x86_64.exe"
$wd = Split-Path -Parent $MyInvocation.MyCommand.Path
$log = Join-Path $wd ("logs\autorecover-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + ".log")
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
    "found log"                 = "found log"
    "image not found"           = "ERROR: image not found"
    "decision reboot"           = "recover decision: reboot (target untouched, STEP=image-not-found)"
    "RESULT FAILED"             = "RESULT: restore FAILED"
    "AUTO-RECOVER line"         = "AUTO-RECOVER: rebooting now"
    "guest rebooted"            = "reboot: Restarting system"
    "no bare shell"             = $true
    "no recovery menu"          = $true
}
$fail = 0
foreach ($k in $checks.Keys) {
    if ($k -eq "no bare shell") {
        $ok = -not ($txt -match "entering rescue shell")
    } elseif ($k -eq "no recovery menu") {
        $ok = -not ($txt -match "SysRecover recovery menu")
    } else {
        $ok = $txt -match [regex]::Escape($checks[$k])
    }
    Write-Output ("  {0,-26} {1}" -f $k, $(if ($ok) { "PASS" } else { "FAIL" }))
    if (-not $ok) { $fail++ }
}
Write-Output "== SR/ZJ/AUTO lines (tail) =="
Select-String -Path $log -Pattern "SR:|ZJ:|AUTO-RECOVER|reboot:" | ForEach-Object { $_.Line } | Select-Object -Last 25
if ($fail -eq 0) { Write-Output "AUTORECOVER DRILL: PASS"; exit 0 }
Write-Output "AUTORECOVER DRILL: FAIL ($fail checks)"; exit 1
