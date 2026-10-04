# prep-cust-disks.ps1 - format the replicas + prime the drill contract.
#
# Why: the rescue auto-runs a restore only when it finds a task. Without any
# _zjresy*.log it falls into the slow "modprobe everything" sweep. The real
# customer disk had C: formatted as NTFS with the contract at its root, so we
# reproduce that: format C: NTFS and (for the drill disk) drop
# _zjresy-drill.log + restore-task.conf onto it. Also formats data partitions.
#
# KEEP THIS FILE PURE ASCII.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\vmtest\prep-cust-disks.ps1

$ErrorActionPreference = "Stop"
$wd = Split-Path -Parent $MyInvocation.MyCommand.Path
$base = Join-Path $wd "base"

$logTxt = @"
action=restore
log_time=2026-10-03 08:23:00
software_version=0.6.18
software_path=X:\ZJRESTORE\SysRecover.exe
target_disk_name=QEMU CUSTDRILL
target_disk_serial=CUSTDRILL0001
target_disk_size=256061562880
target_part_offset=1090539520
target_part_size=254969957888
target_fs=NTFS
target_vol_label=Windows
image_path=Z:/images/test.wim
image_index=1
esp_index=2
repair_boot=1
pt_type=gpt
contract_version=1
software_dir=X:\ZJRESTORE
"@
$confTxt = @"
action=restore
pt_type=gpt
target_offset=1090539520
target_size=254969957888
target_disk_serial=CUSTDRILL0001
image_path=Z:/images/test.wim
image_index=1
esp_index=2
repair_boot=1
contract_version=1
software_dir=X:\ZJRESTORE
"@
$logFile  = Join-Path $base "_zjresy-cust.log"
$confFile = Join-Path $base "cust-restore-task.conf"
Set-Content -Path $logFile  -Value $logTxt  -Encoding ASCII
Set-Content -Path $confFile -Value $confTxt -Encoding ASCII

function DpRun([int]$diskNo, [string[]]$steps, [string]$tag) {
    $sf = Join-Path $env:TEMP ("dp_" + $tag + ".txt")
    $lines = @("select disk $diskNo") + $steps
    Set-Content -Path $sf -Value ($lines -join "`r`n") -Encoding ASCII
    $out = & diskpart.exe /s $sf 2>&1
    $out | Select-String -Pattern "error|failed" -SimpleMatch | ForEach-Object {
        Write-Output ("diskpart[$tag]: " + $_.Line)
    }
}

function MountVhd([string]$path) {
    Mount-DiskImage -ImagePath $path | Out-Null
    return (Get-DiskImage -ImagePath $path | Get-Disk).Number
}

function DismountVhd([string]$path) {
    Dismount-DiskImage -ImagePath $path | Out-Null
}

# --- disk0 drill: C: NTFS + contract on root -------------------------------
$vhd0 = Join-Path $base "custdrill.vhd"
$n = MountVhd $vhd0
Write-Output "custdrill.vhd -> disk $n"
DpRun $n @(
    "select partition 3",
    "format fs=ntfs quick label=Windows",
    "assign letter=W"
) "drill"
if (Test-Path "W:\") {
    Copy-Item $logFile  "W:\_zjresy-cust.log"
    Copy-Item $confFile "W:\restore-task.conf"
    Write-Output "contract copied to W:\"
}
& mountvol.exe W: /d | Out-Null
DismountVhd $vhd0

# --- disk0 clean: C: NTFS ---------------------------------------------------
$vhd1 = Join-Path $base "custdisk0.vhd"
$n = MountVhd $vhd1
Write-Output "custdisk0.vhd -> disk $n"
DpRun $n @(
    "select partition 3",
    "format fs=ntfs quick label=Windows"
) "clean"
DismountVhd $vhd1

# --- data disk: four NTFS partitions ---------------------------------------
$vhd2 = Join-Path $base "custdata.vhd"
$n = MountVhd $vhd2
Write-Output "custdata.vhd -> disk $n"
$labels = @("SOFTWARE", "OFFICE", "DATA", "TESTDISK")
$steps = @()
for ($i = 1; $i -le 4; $i++) {
    $steps += "select partition $i"
    $steps += ("format fs=ntfs quick label=" + $labels[$i - 1])
}
DpRun $n $steps "data"
DismountVhd $vhd2

Write-Output "PREP DONE"
