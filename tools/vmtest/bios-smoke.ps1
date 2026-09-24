# bios-smoke.ps1 - BIOS/GRUB4DOS boot smoke test for the SysRecover rescue layer.
#
# What it proves (no Windows needed):
#   MBR(grldr.mbr) -> \grldr -> \menu.lst -> kernel vmlinuz-zjrestore + our
#   initramfs -> rescue /init runs -> disks enumerated.
# This is the BIOS branch of the product (the UEFI branch has its own smokes).
#
# It builds a small 512MB FAT32 disk (active partition at LBA 2048) and boots it
# with QEMU -boot c. Assertions come from the SERIAL log (console=ttyS0):
#   "SR: modules loaded" and "SR: block devs:" containing sda.
#
# KEEP THIS FILE PURE ASCII (AGENTS.md section 15).
#
# Usage: powershell -File bios-smoke.ps1 [-Secs 120]

param([int]$Secs = 120)

$ErrorActionPreference = "Stop"
$mb = "D:\Prog\ProgIDE\msys64\mingw64\bin"
$qemu = Join-Path $mb "qemu-system-x86_64.exe"
$qimg = Join-Path $mb "qemu-img.exe"
$mfmt = Join-Path $mb "mformat.exe"
$mcp  = Join-Path $mb "mcopy.exe"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$boot = Join-Path $root "bootfiles"
$tmp  = Join-Path $env:TEMP "zj-bios-smoke"
$img  = Join-Path $tmp "bios.raw"
$off  = 1048576            # partition start = LBA 2048 * 512
$psec = 2048
$plen = 512 * 1024 * 1024 / 512 - $psec

foreach ($f in @($qemu, $qimg, $mfmt, $mcp)) {
    if (!(Test-Path $f)) { Write-Host "missing tool: $f"; exit 1 }
}
foreach ($f in @("grldr", "grldr.mbr", "vmlinuz-zjrestore", "initramfs-zjrestore.cpio.gz")) {
    if (!(Test-Path (Join-Path $boot $f))) { Write-Host "missing bootfile: $f (run make package)"; exit 1 }
}

Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

& $qimg create -f raw $img 512M | Out-Null

function Chs([uint32]$lba) {
    $c = [int]([math]::Floor($lba / (255 * 63)))
    $h = [int](($lba / 63) % 255)
    $s = [int]($lba % 63 + 1)
    if ($c -gt 1023) { $c = 1023; $h = 254; $s = 63 }
    $b1 = [int]((($s -bor (($c -shr 8) -shl 6))) -band 0xFF)
    $b2 = [int]($c -band 0xFF)
    return [byte[]]@([byte]$h, [byte]$b1, [byte]$b2)
}

# MBR: grldr.mbr bootstrap first (0..8191), THEN the partition table at 446
# (order matters: grldr.mbr would otherwise zero the partition table).
$fs = [System.IO.File]::Open($img, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite)
try {
    $gr = [System.IO.File]::ReadAllBytes((Join-Path $boot "grldr.mbr"))
    $fs.Write($gr, 0, $gr.Length)
    $entry = New-Object byte[] 16
    $entry[0] = 0x80                                  # active
    $b = Chs $psec;  $entry[1] = $b[0]; $entry[2] = $b[1]; $entry[3] = $b[2]
    $entry[4] = 0x0C                                  # FAT32 LBA
    $e = Chs ($psec + $plen - 1); $entry[5] = $e[0]; $entry[6] = $e[1]; $entry[7] = $e[2]
    $lba = [BitConverter]::GetBytes([uint32]$psec);  [Array]::Copy($lba, 0, $entry, 8, 4)
    $sz  = [BitConverter]::GetBytes([uint32]$plen);  [Array]::Copy($sz, 0, $entry, 12, 4)
    $fs.Seek(446, [System.IO.SeekOrigin]::Begin) | Out-Null
    $fs.Write($entry, 0, 16)
    $fs.Seek(510, [System.IO.SeekOrigin]::Begin) | Out-Null
    $fs.Write([byte[]]@(0x55, 0xAA), 0, 2)
} finally { $fs.Close() }

& $mfmt -i "$img@@$off" -F -v ZJBOOT :: | Out-Null
$menu = "timeout 3`ndefault 0`ntitle zjrestore-test`nkernel /vmlinuz-zjrestore console=tty0 console=ttyS0,115200 loglevel=7 nvme_core.io_timeout=1`ninitrd /initramfs-zjrestore.cpio.gz`n"
Set-Content -Encoding ASCII (Join-Path $tmp "menu.lst") $menu
& $mcp -o -i "$img@@$off" (Join-Path $boot "grldr") ::grldr | Out-Null
& $mcp -o -i "$img@@$off" (Join-Path $tmp "menu.lst") ::menu.lst | Out-Null
& $mcp -o -i "$img@@$off" (Join-Path $boot "vmlinuz-zjrestore") ::vmlinuz-zjrestore | Out-Null
& $mcp -o -i "$img@@$off" (Join-Path $boot "initramfs-zjrestore.cpio.gz") ::initramfs-zjrestore.cpio.gz | Out-Null

$log = Join-Path $env:TEMP "zj-bios-smoke.log"
Remove-Item $log -Force -ErrorAction SilentlyContinue
# -cpu max is REQUIRED for our kernel under TCG (PIT-013).
$p = Start-Process -FilePath $qemu -ArgumentList @(
        "-accel","tcg,thread=multi","-cpu","max","-m","1024",
        "-drive","file=$img,format=raw,if=ide","-boot","c","-no-reboot","-nographic"
     ) -RedirectStandardOutput $log -NoNewWindow -PassThru
Start-Sleep -Seconds $Secs
if (!$p.HasExited) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Seconds 2

$txt = Get-Content $log -Raw -Encoding UTF8
$p1 = $txt.Contains("GRUB4DOS")
$p2 = $txt.Contains("SR: modules loaded")
$p3 = $txt.Contains("SR: block devs:") -and ($txt -match "block devs:.*sda")
Write-Host ("GRUB4DOS menu shown     : {0}" -f $p1)
Write-Host ("rescue init ran         : {0}" -f $p2)
Write-Host ("disks enumerated (sda)  : {0}" -f $p3)
Write-Host ("log                     : $log")
if ($p2 -and $p3) { Write-Host "BIOS SMOKE: PASS"; exit 0 }
Write-Host "BIOS SMOKE: FAIL"
exit 1
