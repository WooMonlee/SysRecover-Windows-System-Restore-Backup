# run-swap-drill.ps1 - A-engine (engine=swap) + ESP/EA subimage restore drill.
#
# Why this exists (PIT-138): with engine=swap the rescue script never enters the
# B-engine branch, so WIMLIB / IMG_FILE used to stay undefined -> `set -u` killed
# the script with rc=2 right at `restore esp subimage idx=2 -> /dev/sda1`.
# The ordinary drill.raw is engine=block and therefore never covers that path.
#
# Two boots of the same disk:
#   boot 1 - menu.lst carries zjsetup=1 -> init runs setup-swap.sh, which
#            mkntfs's sda2, plants the "old system" at its root plus `~new/`
#            (with the .zj-done marker), then rewrites menu.lst to drop the
#            flag and powers off.
#   boot 2 - the real restore: swap engine exchanges ~new into place, then must
#            still find images/test.wim and apply ESP subimage 2 + EA subimage 3.
#
# Build first (admin not required):
#   $env:ZJ_DRILL_ENGINE="swap"
#   <repo>\Python313\python.exe tools\vmtest\mk-drill.py
#
# Then: powershell -ExecutionPolicy Bypass -File tools\vmtest\run-swap-drill.ps1
#
# NOTE: keep this file ASCII-only. PowerShell 5.1 parses .ps1 as ANSI when there
# is no BOM, so CJK comments break the parser.
param([int]$SetupSecs = 150, [int]$RestoreSecs = 200)
$ErrorActionPreference = 'Continue'
$qemu = "D:\Prog\ProgIDE\msys64\mingw64\bin\qemu-system-x86_64.exe"
$wd = Split-Path -Parent $MyInvocation.MyCommand.Path
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'

function Invoke-Boot {
    param([string]$Tag, [int]$Secs)
    $log = Join-Path $wd ("logs\swap-drill-" + $Tag + "-" + $stamp + ".log")
    $p = Start-Process -FilePath $qemu -WorkingDirectory $wd -ArgumentList @(
        "-accel", "tcg,thread=multi", "-cpu", "max", "-m", "1024",
        "-drive", "file=base/drill.raw,format=raw,if=ide",
        "-boot", "c", "-no-reboot", "-nographic"
    ) -RedirectStandardOutput $log -RedirectStandardError ($log + ".err") -PassThru -NoNewWindow
    Write-Output ("[{0}] qemu pid={1} log={2}" -f $Tag, $p.Id, $log)
    $p | Wait-Process -Timeout $Secs -ErrorAction SilentlyContinue
    if (-not $p.HasExited) {
        Write-Output ("[{0}] timeout -> killing" -f $Tag)
        $p.Kill()
    } else {
        Write-Output ("[{0}] qemu exited code={1}" -f $Tag, $p.ExitCode)
    }
    $a = Get-Content $log -Raw -ErrorAction SilentlyContinue
    $b = Get-Content ($log + ".err") -Raw -ErrorAction SilentlyContinue
    return (@($a, $b) -join "`n")
}

# ---- boot 1: setup (build ~new on sda2) ----
$t1 = Invoke-Boot -Tag "setup" -Secs $SetupSecs
foreach ($c in @(
    "setup mode: running setup-swap.sh",
    "SETUP DONE",
    "setup-swap.sh returned (rc=0)"
)) {
    $ok = $t1 -match [regex]::Escape($c)
    Write-Output ("  setup: {0,-42} {1}" -f $c, $(if ($ok) { "PASS" } else { "FAIL" }))
    if (-not $ok) { Write-Output "SWAP DRILL: FAIL (setup boot)"; exit 1 }
}

# ---- boot 2: real restore ----
$t2 = Invoke-Boot -Tag "restore" -Secs $RestoreSecs
$checks = [ordered]@{
    "engine=swap read"             = "engine=swap"
    "PIT-138 image re-scan"        = "found image"
    "swap started"                 = "A-engine swap: ~new -> root"
    "swap rc=0"                    = "swap rc=0"
    "swap done"                    = "swap done: ~new -> root, old -> ~old"
    "ESP subimage apply starts"    = "restore esp subimage idx=2 -> /dev/sda1"
    "ESP subimage restored"        = "esp restored (rc=0)"
    "EA subimage picked"           = "ea subimage: index=3"
    "RESTORE DONE"                 = "RESTORE DONE: /dev/sda2"
}
$fail = 0
foreach ($k in $checks.Keys) {
    $ok = $t2 -match [regex]::Escape($checks[$k])
    Write-Output ("  {0,-30} {1}" -f $k, $(if ($ok) { "PASS" } else { "FAIL" }))
    if (-not $ok) { $fail++ }
}
# The PIT-138 symptom itself: set -u aborting the script with rc=2.
$rc2 = $t2 -match [regex]::Escape("restore script returned (rc=2)")
Write-Output ("  {0,-30} {1}" -f "no set -u rc=2 abort", $(if (-not $rc2) { "PASS" } else { "FAIL" }))
if ($rc2) { $fail++ }

Write-Output "== SR/ZJ lines (tail) =="
@($t2 -split "`n") | Select-String -Pattern "SR:|ZJ:|SWAP:|Kernel panic|reboot:|engine=" |
    ForEach-Object { $_.Line } | Select-Object -Last 40
if ($fail -eq 0) { Write-Output "SWAP DRILL: PASS"; exit 0 }
Write-Output "SWAP DRILL: FAIL ($fail checks)"; exit 1
