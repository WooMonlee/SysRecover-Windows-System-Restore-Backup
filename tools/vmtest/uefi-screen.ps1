# uefi-screen.ps1 - Verify the rescue kernel has a WORKING on-screen console under
# UEFI (the "black screen looks like a hang" regression, PIT-061).
#
# Method (deterministic): boot our kernel via the OVMF shell with BOTH
#   console=tty0 console=ttyS0
# and assert the SERIAL log contains the framebuffer-console bring-up lines:
#       efifb: probing for efifb
#       Console: switching to colour frame buffer device ...
#       fb0: EFI VGA frame buffer device
# If the fbdev console were missing we would instead see
#   "Console: switching to colour dummy device" / no fb0 -> FAIL.
#
# History: this test used to compare two screendumps (identical = frozen). That
# became a FALSE FAIL after the PIT-079 module trim: the rescue now finishes
# faster and sits at a static "#" prompt before the first shot. Serial markers are
# timing-independent. (2026-09-24)
#
# KEEP THIS FILE PURE ASCII (AGENTS.md section 15).
#
# Usage: powershell -File uefi-screen.ps1 [-Secs 90]

param([int]$Secs = 90)

$ErrorActionPreference = "Stop"
$qemu = "D:\Prog\ProgIDE\msys64\mingw64\bin\qemu-system-x86_64.exe"
$ovmf = "D:\Prog\ProgIDE\msys64\mingw64\share\qemu\edk2-x86_64-code.fd"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$boot = Join-Path $root "bootfiles"
$tmp  = Join-Path $env:TEMP "zj-uefi-screen"

if (!(Test-Path $qemu)) { Write-Host "qemu not found"; exit 1 }
if (!(Test-Path $ovmf)) { Write-Host "OVMF not found"; exit 1 }

Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path (Join-Path $tmp "EFI\ZJRESTORE") | Out-Null
Copy-Item (Join-Path $boot "vmlinuz-zjrestore") (Join-Path $tmp "EFI\ZJRESTORE\vmlinuz-zjrestore.efi")
Copy-Item (Join-Path $boot "initramfs-zjrestore.cpio.gz") (Join-Path $tmp "EFI\ZJRESTORE\initramfs-zjrestore.cpio.gz")
$nsh = "fs0:\EFI\ZJRESTORE\vmlinuz-zjrestore.efi initrd=\EFI\ZJRESTORE\initramfs-zjrestore.cpio.gz console=tty0 console=ttyS0,115200 loglevel=7 nvme_core.io_timeout=1`r`n"
Set-Content -Encoding ASCII (Join-Path $tmp "startup.nsh") $nsh

$log = Join-Path $env:TEMP "zj-uefi-screen.log"
Remove-Item $log -Force -ErrorAction SilentlyContinue
$qargs = @("-m","1024","-smp","2",
           "-drive","if=pflash,format=raw,unit=0,readonly=on,file=$ovmf",
           "-drive","file=fat:rw:$tmp,format=raw,if=ide,index=0",
           "-nographic","-no-reboot")
$p = Start-Process -FilePath $qemu -ArgumentList $qargs -RedirectStandardOutput $log -NoNewWindow -PassThru
Start-Sleep -Seconds $Secs
if (!$p.HasExited) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Seconds 2

$txt = Get-Content $log -Raw -Encoding UTF8
$p1 = $txt.Contains("fb0: EFI VGA frame buffer device")
$p2 = $txt.Contains("Console: switching to colour frame buffer device")
$p3 = $txt.Contains("efifb: probing for efifb")
Write-Host ("efifb probed            : {0}" -f $p3)
Write-Host ("fbcon switched (colour) : {0}" -f $p2)
Write-Host ("fb0 registered          : {0}" -f $p1)
Write-Host ("log                     : $log")
if ($p1 -and $p2) { Write-Host "UEFI SCREEN: PASS (framebuffer console is up)"; exit 0 }
Write-Host "UEFI SCREEN: FAIL (no framebuffer console -> PIT-061 regression)"
exit 1
