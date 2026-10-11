# mk-swap-ntfs.ps1 - Create NTFS partition image for swap drill.
# Output: graveyard\tools\vmtest\base\swap-ntfs.img (raw partition data)
# Usage: powershell -ExecutionPolicy Bypass -File graveyard\tools\vmtest\mk-swap-ntfs.ps1
param()
$ErrorActionPreference = 'Stop'
$wd = Split-Path -Parent $MyInvocation.MyCommand.Path
$vhd = Join-Path $wd 'base\_tmp_ntfs.vhd'
$out = Join-Path $wd 'base\swap-ntfs.img'
$sizeMB = 683
$dpScript = Join-Path $wd 'base\_dp.txt'

# Cleanup
if (Test-Path $vhd) {
    try { Dismount-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue } catch {}
    Remove-Item $vhd -Force -ErrorAction SilentlyContinue
}
if (Test-Path $out) { Remove-Item $out -Force }

# Use diskpart to create VHD (works without Hyper-V module)
$dpContent = @"
create vdisk file="$vhd" maximum=$sizeMB type=fixed
select vdisk file="$vhd"
attach vdisk
create partition primary
format fs=ntfs quick label=ZJSYS
exit
"@
Set-Content -Path $dpScript -Value $dpContent -Encoding ASCII
$dpResult = diskpart /s $dpScript
Remove-Item $dpScript -Force
Write-Host "diskpart output:"
$dpResult | ForEach-Object { Write-Host "  $_" }

# Find the partition (VHD-backed disk has no drive letter yet)
$part = Get-Partition | Where-Object { $_.DriveLetter -eq $null -and $_.Size -gt 700MB -and $_.Type -eq 'Basic' } | Sort-Object DiskNumber -Descending | Select-Object -First 1
if (-not $part) { throw "Cannot find VHD partition" }
$diskNum = $part.DiskNumber
Write-Host "Found partition: disk=$diskNum part=$($part.PartitionNumber)"

# Assign drive letter Z via diskpart
$dpScript2 = Join-Path $wd 'base\_dp2.txt'
@"
select disk $diskNum
select partition $($part.PartitionNumber)
assign letter=Z
exit
"@ | Set-Content -Path $dpScript2 -Encoding ASCII
diskpart /s $dpScript2 | Out-Null
Remove-Item $dpScript2 -Force

# Wait for Z:
$drive = 'Z:\'
for ($i = 0; $i -lt 20; $i++) {
    if (Test-Path $drive) { break }
    Start-Sleep -Milliseconds 500
}
if (-not (Test-Path $drive)) { throw "Drive Z: not available" }

try {
    # Old system files at root
    New-Item -ItemType Directory -Path "$drive\Windows\System32" -Force | Out-Null
    $oldKernel = [byte[]]::new(1100)
    for ($i = 0; $i -lt 100; $i++) { $s = [Text.Encoding]::ASCII.GetBytes('OLD_KERNEL'); $oldKernel[$i*11..($i*11+10)] = $s }
    [IO.File]::WriteAllBytes("$drive\Windows\System32\ntoskrnl.exe", $oldKernel)
    $oldBoot = [byte[]]::new(550)
    for ($i = 0; $i -lt 50; $i++) { $s = [Text.Encoding]::ASCII.GetBytes('OLD_BOOTMGR'); $oldBoot[$i*11..($i*11+10)] = $s }
    [IO.File]::WriteAllBytes("$drive\bootmgr", $oldBoot)
    Set-Content -Path "$drive\old_userfile.txt" -Value "old user data" -Encoding ASCII

    # ~new: new system
    New-Item -ItemType Directory -Path "$drive\~new\Windows\System32" -Force | Out-Null
    $newKernel = [byte[]]::new(1000)
    for ($i = 0; $i -lt 100; $i++) { $s = [Text.Encoding]::ASCII.GetBytes('NEW_KERNEL'); $newKernel[$i*10..($i*10+9)] = $s }
    [IO.File]::WriteAllBytes("$drive\~new\Windows\System32\ntoskrnl.exe", $newKernel)
    $newBoot = [byte[]]::new(550)
    for ($i = 0; $i -lt 50; $i++) { $s = [Text.Encoding]::ASCII.GetBytes('NEW_BOOTMGR'); $newBoot[$i*11..($i*11+10)] = $s }
    [IO.File]::WriteAllBytes("$drive\~new\bootmgr", $newBoot)
    Set-Content -Path "$drive\~new\new_userfile.txt" -Value "new user data" -Encoding ASCII

    # .zj-done marker
    $ts = [DateTimeOffset]::Now.ToUnixTimeSeconds()
    Set-Content -Path "$drive\~new\.zj-done" -Value "1024 $ts" -Encoding ASCII

    Write-VolumeCache -DriveLetter Z
    Start-Sleep -Seconds 1
    Write-Host "Populated: old system + ~new + .zj-done"
} finally {
    # Unassign and detach
    $dpScript3 = Join-Path $wd 'base\_dp3.txt'
    @"
select disk $diskNum
select partition 1
remove letter=Z
exit
"@ | Set-Content -Path $dpScript3 -Encoding ASCII
    diskpart /s $dpScript3 | Out-Null
    Remove-Item $dpScript3 -Force
    $dpScript4 = Join-Path $wd 'base\_dp4.txt'
    @"
select vdisk file="$vhd"
detach vdisk
exit
"@ | Set-Content -Path $dpScript4 -Encoding ASCII
    diskpart /s $dpScript4 | Out-Null
    Remove-Item $dpScript4 -Force
}

# Extract raw partition data from fixed VHD (data at offset 0)
$fs = [IO.File]::OpenRead($vhd)
$partBytes = New-Object byte[] ($sizeMB * 1MB)
$fs.Read($partBytes, 0, $partBytes.Length) | Out-Null
$fs.Close()
[IO.File]::WriteAllBytes($out, $partBytes)
Remove-Item $vhd -Force
Write-Host "Wrote $out ($([math]::Round($partBytes.Length/1MB)) MB)"
