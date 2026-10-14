# run-undo-drill.ps1 - A-engine "return to old system" (action=returnold) drill.
#
# Why this exists (PIT-139, plan C): the reverse swap (root -> ~/.new discarded,
# ~/.old -> root) lives entirely in the rescue layer, so the ordinary drill.raw
# (engine=block) and the forward-swap drill never cover it.
#
# Two boots of the same disk:
#   boot 1 - menu.lst carries zjsetup=2 (or 3 with -Resume) -> init runs
#            setup-swap.sh, which mkntfs's sda2 and builds the post-swap layout
#            directly (new system at root + ~/.old/ = old system + .zj-keepold),
#            then rewrites menu.lst to drop the flag and powers off.
#            The host (Windows) cannot mount the NTFS the rescue layer just
#            wrote, so there is no way to change the contract between two boots;
#            building the post-swap state up front is the way around that.
#   boot 2 - the real reverse swap: contract action=returnold -> the rescue
#            script runs zjswap-undo.sh, which must NOT touch the image, must
#            NOT mkntfs, and must put the old system back at the root.
#
# -Resume (T3) builds an "interrupted reverse swap" instead: ~/.new already
# holds the new system with phase=2 (L5 done, L6 not yet), so the script must
# say it is resuming, must SKIP L5, and must still put the old system back.
# Skipping L5 is the whole point - if it re-ran, it would move the old files
# that L6 already put at the root back into ~/.new and L7 would silently
# delete them (the bug found while writing this drill).
#
# Build first (admin not required):
#   $env:ZJ_DRILL_UNDO="1"          # or ZJ_DRILL_UNDORESUME="1" for -Resume
#   D:\Prog\_Project\SysRecover\Python313\python.exe tools\vmtest\mk-drill.py
#   (set ZJ_DRILL_ENGINE/ZJ_DRILL_UNDO/ZJ_DRILL_UNDORESUME before EVERY mk-drill
#    call - the drill disk is rebuilt from scratch each time.)
#
# Then: powershell -ExecutionPolicy Bypass -File tools\vmtest\run-undo-drill.ps1 [-Resume] [-Bulk N]
#
# -Bulk N (PIT-142, scale drill): rebuild the disk with N small files planted in
#   the tree the reverse swap must rename/delete, then run the drill.  The toy
#   3-file tree could never reproduce "rm -rf ~/.new failed" (2026-10-13); N in
#   the tens of thousands makes the metadata path realistic.  Implies a rebuild,
#   because mk-drill.py reads the env every time.
#
# NOTE: keep this file ASCII-only. PowerShell 5.1 parses .ps1 as ANSI when there
# is no BOM, so CJK comments break the parser.
param([switch]$Resume, [int]$SetupSecs = 150, [int]$RestoreSecs = 120, [int]$Bulk = 0)
$ErrorActionPreference = 'Continue'
$qemu = "D:\Prog\ProgIDE\msys64\mingw64\bin\qemu-system-x86_64.exe"
$wd = Split-Path -Parent $MyInvocation.MyCommand.Path
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$mode = if ($Resume) { "3" } else { "2" }
$tag0 = if ($Resume) { "undo-resume" } else { "undo" }
if ($Bulk -gt 0) {
    $tag0 = $tag0 + "-bulk" + $Bulk
    # Scale the timeouts with the tree: ~60s of extra QEMU TCG time per 20k files
    # on top of the base, so a 100k-file run does not get killed mid-rm.
    $SetupSecs   = $SetupSecs   + [int][math]::Ceiling($Bulk / 20000.0) * 60
    $RestoreSecs = $RestoreSecs + [int][math]::Ceiling($Bulk / 20000.0) * 90
    $env:ZJ_DRILL_BULK = "$Bulk"
} else {
    # mk-drill.py tests this with != '' -- "0" would still be True.
    Remove-Item Env:ZJ_DRILL_BULK -ErrorAction SilentlyContinue
}
# ALWAYS rebuild.  A previous run rewrites menu.lst (setup-swap.sh seds out
# zjsetup=), so reusing the disk makes the next setup boot silently do nothing
# and every assertion then fails against a log that never had those lines.
if ($Resume) { $env:ZJ_DRILL_UNDORESUME = "1"; $env:ZJ_DRILL_UNDO = "1" }
else {
    Remove-Item Env:ZJ_DRILL_UNDORESUME -ErrorAction SilentlyContinue
    $env:ZJ_DRILL_UNDO = "1"
}
Write-Output ("[build] drill disk mode={0} bulk={1} (setup={2}s restore={3}s)" -f `
    $mode, $Bulk, $SetupSecs, $RestoreSecs)
& "D:\Prog\_Project\SysRecover\Python313\python.exe" (Join-Path $wd 'mk-drill.py')
if ($LASTEXITCODE -ne 0) { Write-Output "UNDO DRILL: FAIL (mk-drill.py)"; exit 1 }

function Invoke-Boot {
    param([string]$Tag, [int]$Secs)
    $log = Join-Path $wd ("logs\undo-drill-" + $Tag + "-" + $stamp + ".log")
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

# ---- assertion helpers ------------------------------------------------------
# NOTE: these two MUST print via Write-Host, never Write-Output.
# Write-Output joins the function's pipeline output with the returned bool, so
# the caller's `-not (Assert-...)` always evaluates a non-empty array (truthy)
# and $fail never increments -- the drill printed PASS even when the log
# contained zero SWAPUNDO lines.  Caught 2026-10-13.  See PIT-142.
function Assert-Present {
    param([string]$Text, [string]$What, [string]$Needle)
    $ok = $Text -match [regex]::Escape($Needle)
    Write-Host ("  {0,-38} {1}" -f $What, $(if ($ok) { "PASS" } else { "FAIL" }))
    return $ok
}

function Assert-Absent {
    param([string]$Text, [string]$What, [string]$Needle)
    $ok = -not ($Text -match [regex]::Escape($Needle))
    Write-Host ("  {0,-38} {1}" -f $What, $(if ($ok) { "PASS" } else { "FAIL (present)" }))
    return $ok
}

# ---- boot 1: setup ----
$t1 = Invoke-Boot -Tag ("setup-" + $tag0) -Secs $SetupSecs
$fail = 0
foreach ($c in @(
    "setup mode: running setup-swap.sh (mode=$mode)",
    "SETUP DONE",
    "setup-swap.sh returned (rc=0)"
)) {
    if (-not (Assert-Present $t1 $c $c)) { $fail++ }
}
if ($Resume) {
    if (-not (Assert-Present $t1 "interrupted returnold planted" "setup done (mode=3)")) { $fail++ }
} else {
    if (-not (Assert-Present $t1 "post-swap layout planted" "setup done (mode=2)")) { $fail++ }
}
if ($Bulk -gt 0) {
    $bl = "bulk tree: $Bulk files"
    if (-not (Assert-Present $t1 "bulk tree planted ($Bulk files)" $bl)) { $fail++ }
}
if ($fail -ne 0) { Write-Output "UNDO DRILL: FAIL (setup boot)"; exit 1 }

# ---- boot 2: reverse swap ----
$t2 = Invoke-Boot -Tag ($tag0) -Secs $RestoreSecs
Write-Output "-- assertions ($tag0) --"
# Present
foreach ($pair in @(
    @("returnold read from contract",  "action=returnold: reverse swap on target"),
    @("undo script ran",               "SWAPUNDO:"),
    @("undo rc=0",                     "return-old rc=0"),
    @("post-check ok (real disk)",     "post-check ok: old system at root"),
    @("RETURN-OLD DONE",               "RETURN-OLD DONE: old system back at root"),
    # init prints "RESULT: restore finished ok (rc=0)" only when its capture
    # survived; that line is init-side and flaky in short log captures. The
    # script's own authoritative success markers (above + rc=0) are what we
    # assert on. Keep RESULT as a soft signal via Assert-Any below.
    @("script rc=0",                   "restore script returned (rc=0)")
)) {
    if (-not (Assert-Present $t2 $pair[0] $pair[1])) { $fail++ }
}
# Soft: RESULT line is init-side and its absence is not a real failure. Do not
# fail the drill on it; just report. (Real success = RETURN-OLD DONE + rc=0 above.)
$hasResult = $t2 -match "RESULT: restore finished ok"
Write-Host ("  {0,-38} {1}" -f "RESULT ok (soft)", $(if ($hasResult) { "PASS" } else { "NOTE (absent, non-fatal)" }))
# Absent - the path must not fall through into the B-engine restore.
# Needles must be specific enough not to match our own status lines. "mkntfs"
# alone hits the returnold banner "(no image, no mkntfs, no apply)" -> use the
# real execution form "mkntfs /dev/" (format step always names the device).
foreach ($pair in @(
    @("no mkntfs",                     "mkntfs /dev/"),
    @("no image scan",                 "found image"),
    @("no apply",                      "apply rc="),
    @("no set -u rc=2",                "restore script returned (rc=2)"),
    @("no B-engine swap",              "A-engine swap:"),
    @("no post-check failure",         "post-check failed")
)) {
    if (-not (Assert-Absent $t2 $pair[0] $pair[1])) { $fail++ }
}
# Resume-only: L5 must be skipped (phase>=2) or the old files L6 already put at
# the root would be moved back into ~/.new and silently deleted by L7.
if ($Resume) {
    if (-not (Assert-Present $t2 "resume detected"     "SWAPUNDO: resume: ~/.new/.zj-undo-phase phase=2")) { $fail++ }
    if (-not (Assert-Absent $t2 "L5 skipped"           "phase=2 (root moved into ~/.new)")) { $fail++ }
} else {
    if (-not (Assert-Present $t2 "phase=1 marker"      "SWAPUNDO: marker: ~/.new/.zj-undo-phase phase=1")) { $fail++ }
    if (-not (Assert-Present $t2 "L5 done (phase=2)"   "phase=2 (root moved into ~/.new)")) { $fail++ }
}

Write-Output "== SWAPUNDO/SR/ZJ lines (tail) =="
@($t2 -split "`n") | Select-String -Pattern "SWAPUNDO:|SR:|ZJ:|RESULT:|Kernel panic|reboot:" |
    ForEach-Object { $_.Line } | Select-Object -Last 40

if ($fail -eq 0) { Write-Output ("UNDO DRILL ({0}): PASS" -f $tag0); exit 0 }
Write-Output ("UNDO DRILL ({0}): FAIL ({1} checks)" -f $tag0, $fail); exit 1
