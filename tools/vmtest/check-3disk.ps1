# check-3disk.ps1 - mount the 3-disk VHDs and report black-box artifacts.
#
# After a run (success OR failure) this proves what actually landed on disk:
# ZJRESTORE-status.txt (receipt), ZJRESTORE-last.log / probe, ZJRESTORE-logs\
# (fallback home), software logs dir, and any _zjresy*/zjrestore-* leftovers.
#
# KEEP THIS FILE PURE ASCII (AGENTS.md section 15).
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\vmtest\check-3disk.ps1

$ErrorActionPreference = "Continue"
$wd = Split-Path -Parent $MyInvocation.MyCommand.Path
$base = Join-Path $wd "base\3disk"
$vhds = @("disk0-ma.vhd", "disk1-st1000.vhd", "disk2-kioxia.vhd")

foreach ($v in $vhds) {
    $p = Join-Path $base $v
    if (!(Test-Path $p)) { Write-Output "missing $p"; continue }
    Write-Output "==== $v ===="
    Mount-DiskImage -ImagePath $p | Out-Null
    $d = Get-DiskImage -ImagePath $p | Get-Disk
    if ($d.IsOffline) { Set-Disk -Number $d.Number -IsOffline $false }
    $n = $d.Number
    $temp = @()
    $parts = Get-Partition -DiskNumber $n -ErrorAction SilentlyContinue
    foreach ($part in $parts) {
        # 没有文件系统的分区（MSR / 扩展容器）跳过：给它们分配盘符会报错
        $vol = Get-Volume -Partition $part -ErrorAction SilentlyContinue
        if (-not $vol -or -not $vol.FileSystem) { continue }
        $letter = $part.DriveLetter
        if (-not $letter) {
            foreach ($cand in 'Y', 'Z', 'V', 'U', 'T') {
                if (-not (Test-Path ($cand + ':\'))) {
                    try {
                        Add-PartitionAccessPath -DiskNumber $n -PartitionNumber $part.PartitionNumber -AccessPath ($cand + ':\') -ErrorAction Stop
                        $letter = [char]$cand
                        $temp += $cand
                        break
                    } catch { }
                }
            }
        }
        if (-not $letter) { continue }
        $root = [string]$letter + ":\"
        $found = Get-ChildItem -LiteralPath $root -Recurse -Depth 3 -ErrorAction SilentlyContinue |
            Where-Object { -not $_.PSIsContainer -and ($_.Name -like "ZJRESTORE-*" -or $_.Name -like "zjrestore-*" -or $_.Name -like "_zjresy*" -or $_.Name -eq "restore-task.conf") }
        foreach ($f in $found) {
            Write-Output ("  [p{0}] {1} ({2} bytes)" -f $part.PartitionNumber, $f.FullName.Substring(2), $f.Length)
            if ($f.Name -eq "ZJRESTORE-status.txt") {
                Write-Output "    --- status:"
                Get-Content -LiteralPath $f.FullName -ErrorAction SilentlyContinue | ForEach-Object { Write-Output ("      " + $_) }
            }
            if ($f.Name -in @("ZJRESTORE-last.log", "zjrestore-debug.log")) {
                Write-Output "    --- tail:"
                Get-Content -LiteralPath $f.FullName -Tail 5 -ErrorAction SilentlyContinue | ForEach-Object { Write-Output ("      " + $_) }
            }
        }
        $zl = Join-Path $root "ZJRESTORE-logs"
        if (Test-Path $zl) {
            Write-Output ("  ZJRESTORE-logs dir: " + ((Get-ChildItem $zl | Select-Object -ExpandProperty Name) -join ", "))
        }
    }
    foreach ($c in $temp) { & mountvol.exe ($c + ':') /d 2>$null | Out-Null }
    Dismount-DiskImage -ImagePath $p | Out-Null
}
Write-Output "CHECK3 DONE"
