# prep-3disk.ps1 - format the 3-disk replicas + drop the customer content.
#
# Why: rescue runs automatically only when it finds a task; the replica target
# must carry _zjresy041404.log + restore-task.conf (customer originals) and the
# F: partition must carry the software dir + image (Chinese names).
#
# KEEP THIS FILE PURE ASCII (AGENTS.md section 15).
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\vmtest\prep-3disk.ps1

$ErrorActionPreference = "Stop"
$wd = Split-Path -Parent $MyInvocation.MyCommand.Path
$base = Join-Path $wd "base\3disk"

function MountVhd([string]$path) {
    Mount-DiskImage -ImagePath $path | Out-Null
    $d = Get-DiskImage -ImagePath $path | Get-Disk
    if ($d.IsOffline)  { Set-Disk -Number $d.Number -IsOffline $false }
    if ($d.IsReadOnly) { Set-Disk -Number $d.Number -IsReadOnly $false }
    return $d.Number
}
function DismountVhd([string]$path) { Dismount-DiskImage -ImagePath $path | Out-Null }
function FmtPart([int]$n, [int]$p, [string]$label) {
    Format-Volume -Partition (Get-Partition -DiskNumber $n -PartitionNumber $p) `
        -FileSystem NTFS -NewFileSystemLabel $label -Confirm:$false | Out-Null
}
function ClearW { & mountvol.exe W: /d 2>$null | Out-Null }
function UseW([int]$n, [int]$p) {
    ClearW
    $part = Get-Partition -DiskNumber $n -PartitionNumber $p
    if ($part.DriveLetter -eq 0) {
        Add-PartitionAccessPath -DiskNumber $n -PartitionNumber $p -AccessPath "W:\"
    } else {
        Set-Partition -DiskNumber $n -PartitionNumber $p -NewDriveLetter W
    }
}
function ReleaseW { & mountvol.exe W: /d 2>$null | Out-Null }

# ST1000: D(1) F(5) G(6); stage -> F
$v = Join-Path $base "disk1-st1000.vhd"
$n = MountVhd $v
Write-Output "ST1000 -> disk $n"
Get-Partition -DiskNumber $n | Select-Object PartitionNumber, Offset, Size |
    Format-Table -HideTableHeaders | Out-String | Write-Output
FmtPart $n 1 "D"; FmtPart $n 2 "F"; FmtPart $n 3 "G"
UseW $n 2
Copy-Item -Path (Join-Path $base "stage\*") -Destination "W:\" -Recurse -Force
Write-Output ("F content: " + ((Get-ChildItem "W:\" | Select-Object -ExpandProperty Name) -join ", "))
ReleaseW
DismountVhd $v

# MA: C(1) H(2) I(3)
$v = Join-Path $base "disk0-ma.vhd"
$n = MountVhd $v
Write-Output "MA -> disk $n"
FmtPart $n 1 "Windows"; FmtPart $n 2 "H"; FmtPart $n 3 "I"
DismountVhd $v

# KIOXIA: data partition 3 -> contract at root
$v = Join-Path $base "disk2-kioxia.vhd"
$n = MountVhd $v
Write-Output "KIOXIA -> disk $n"
FmtPart $n 3 "Windows"
UseW $n 3
Copy-Item (Join-Path $base "bundle\_zjresy041404.log") "W:\"
Copy-Item (Join-Path $base "bundle\restore-task.conf") "W:\"
Write-Output ("target root: " + ((Get-ChildItem "W:\" | Select-Object -ExpandProperty Name) -join ", "))
ReleaseW
DismountVhd $v
Write-Output "PREP3 DONE"
