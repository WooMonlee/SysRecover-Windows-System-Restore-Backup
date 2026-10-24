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
# ZJ_EXPECT_REPARSE=scan|cert|stale|oldcert|reject selects the 1019-case
# precheck assertions (default "scan" strips nothing - matches the default drill
# where the contract has no reparse keys, so the fallback metadata scan must
# run). "oldcert" = 0.7.70-era certificate (reparse_scan=0, no efs_scan): the
# rescue layer must ignore the fast path and scan. "reject" additionally
# asserts the negative side: mkntfs/apply must NOT run.
#
# ZJ_EXPECT_EFS=observe|reject selects the W2 EFS assertions (drill image =
# images/efs-test.wim, built by mk-drill with ZJ_DRILL_EFS=1):
#   observe = pre-W2 behavior: normal apply, wimlib only warns about EFS
#   reject  = W2: refuse with STEP=efs-encrypted BEFORE mkntfs/apply
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
$rep = $env:ZJ_EXPECT_REPARSE
if (-not $rep) { $rep = "scan" }
$efs = $env:ZJ_EXPECT_EFS
$checks = [ordered]@{}
$mustNot = [ordered]@{}
if ($efs -eq "reject") {
    $checks["found log + offset match"] = "offset match 105906176"
    $checks["target from log (verified)"] = "target from log location: /dev/sda2 (offset verified)"
    $checks["EFS reject ERROR"] = "image contains EFS-encrypted content the Linux rescue cannot restore"
    $checks["encrypted count listed"] = "encrypted entries:"
    $checks["retry guidance shown"] = "restore then reboot"
    $checks["restore failed rc=1"] = "restore script returned (rc=1)"
    $checks["auto reboot decision"] = "recover decision: reboot"
    $mustNot["mkntfs NOT run"] = "mkntfs /dev/sda2"
    $mustNot["apply NOT run"] = "apply rc=0"
    $mustNot["no RESTORE DONE"] = "RESTORE DONE"
} elseif ($efs -eq "observe") {
    $checks["found log + offset match"] = "offset match 105906176"
    $checks["target from log (verified)"] = "target from log location: /dev/sda2 (offset verified)"
    $checks["mkntfs start_lba=206848"] = "mkntfs /dev/sda2 (start_lba=206848)"
    $checks["apply rc=0"] = "apply rc=0"
    $checks["RESTORE DONE"] = "RESTORE DONE: /dev/sda2"
} elseif ($rep -eq "reject") {
    $checks["found log + offset match"] = "offset match 105906176"
    $checks["target from log (verified)"] = "target from log location: /dev/sda2 (offset verified)"
    $checks["reparse reject ERROR"] = "image contains reparse point(s) the Linux rescue cannot write"
    $checks["bad tag listed"] = "bad tag 0xdead"
    $checks["retry guidance shown"] = "restore then reboot"
    $checks["restore failed rc=1"] = "restore script returned (rc=1)"
    $checks["auto reboot decision"] = "recover decision: reboot"
    $mustNot["mkntfs NOT run"] = "mkntfs /dev/sda2"
    $mustNot["apply NOT run"] = "apply rc=0"
    $mustNot["no RESTORE DONE"] = "RESTORE DONE"
} else {
    $checks["found log + offset match"] = "offset match 105906176"
    $checks["target from log (verified)"] = "target from log location: /dev/sda2 (offset verified)"
    $checks["mkntfs start_lba=206848"] = "mkntfs /dev/sda2 (start_lba=206848)"
    $checks["apply rc=0"] = "apply rc=0"
    $checks["ESP subimage restored"] = "esp restored (rc=0)"
    $checks["EA subimage picked"] = "ea subimage: index=3"
    $checks["EA deployed + hook"] = "ea: deployed (payload + GPO hook"
    $checks["B boot cleanup deployed"] = "boot cleanup: deployed (A-engine-style helper via startup folder)"
    $checks["blackbox to ESP"] = "blackbox: log -> /dev/sda1 (ESP)"
    $checks["RESTORE DONE"] = "RESTORE DONE: /dev/sda2"
    if ($rep -eq "cert") {
        $checks["reparse certified (fast path)"] = "reparse precheck: certified clean by staging side"
        $mustNot["no metadata scan"] = "scanning image metadata"
    } elseif ($rep -eq "stale") {
        $checks["size mismatch -> scan"] = "reparse precheck: recorded size"
        $checks["reparse scan ok"] = "reparse precheck: ok (entries="
    } elseif ($rep -eq "oldcert") {
        $checks["old cert -> full scan"] = "reparse precheck: scanning image metadata"
        $checks["reparse scan ok"] = "reparse precheck: ok (entries="
        $mustNot["no fast path (efs_scan missing)"] = "certified clean by staging side"
    } else {
        $checks["reparse scan ok"] = "reparse precheck: ok (entries="
    }
}
$fail = 0
foreach ($k in $checks.Keys) {
    $ok = $txt -match [regex]::Escape($checks[$k])
    Write-Output ("  {0,-26} {1}" -f $k, $(if ($ok) { "PASS" } else { "FAIL" }))
    if (-not $ok) { $fail++ }
}
foreach ($k in $mustNot.Keys) {
    $bad = $txt -match [regex]::Escape($mustNot[$k])
    Write-Output ("  {0,-26} {1}" -f $k, $(if ($bad) { "FAIL" } else { "PASS" }))
    if ($bad) { $fail++ }
}
Write-Output "== SR/ZJ lines (tail) =="
Select-String -Path $log -Pattern "SR:|ZJ:|Kernel panic|reboot:" | ForEach-Object { $_.Line } | Select-Object -Last 30
if ($fail -eq 0) { Write-Output "DRILL: PASS"; exit 0 }
Write-Output "DRILL: FAIL ($fail checks)"; exit 1
