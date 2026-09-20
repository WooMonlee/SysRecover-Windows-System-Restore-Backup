# uefi-smoke.ps1 - UEFI(OVMF) smoke test for the SysRecover rescue layer.
#
# What it proves (no Windows needed):
#   OVMF(UEFI) -> UEFI Shell -> our kernel (EFI stub) launched with initrd=
#   -> Alpine rescue /init runs.
# It stages the kernel+initramfs on a FAT volume, boots it via the OVMF built-in
# shell's startup.nsh (which passes "initrd=..."), and asserts the serial log
# contains a marker printed by our rescue init ("SR: modules loaded").
#
# Usage: powershell -File uefi-smoke.ps1 [-Secs 150]

param([int]$Secs = 150)

$ErrorActionPreference = "Stop"
$qemu = "D:\Prog\ProgIDE\msys64\mingw64\bin\qemu-system-x86_64.exe"
$ovmf = "D:\Prog\ProgIDE\msys64\mingw64\share\qemu\edk2-x86_64-code.fd"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$boot = Join-Path $root "bootfiles"
$tmp  = Join-Path $env:TEMP "zj-uefi-smoke"

if (!(Test-Path $qemu)) { Write-Host "qemu not found: $qemu"; exit 1 }
if (!(Test-Path $ovmf)) { Write-Host "OVMF not found: $ovmf"; exit 1 }

Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path (Join-Path $tmp "EFI\ZJRESTORE") | Out-Null
Copy-Item (Join-Path $boot "vmlinuz-zjrestore") (Join-Path $tmp "EFI\ZJRESTORE\vmlinuz-zjrestore.efi")
Copy-Item (Join-Path $boot "initramfs-zjrestore.cpio.gz") (Join-Path $tmp "EFI\ZJRESTORE\initramfs-zjrestore.cpio.gz")
$nsh = "fs0:\EFI\ZJRESTORE\vmlinuz-zjrestore.efi initrd=\EFI\ZJRESTORE\initramfs-zjrestore.cpio.gz console=ttyS0 nvme_core.io_timeout=1`r`n"
Set-Content -Encoding ASCII (Join-Path $tmp "startup.nsh") $nsh

$log = Join-Path $env:TEMP "zj-uefi-smoke.log"
Remove-Item $log -Force -ErrorAction SilentlyContinue
$qargs = @("-m","1024","-smp","2",
           "-drive","if=pflash,format=raw,unit=0,readonly=on,file=$ovmf",
           "-drive","file=fat:rw:$tmp,format=raw,if=ide,index=0",
           "-nographic","-no-reboot")
$p = Start-Process -FilePath $qemu -ArgumentList $qargs -RedirectStandardOutput $log -NoNewWindow -PassThru
Start-Sleep -Seconds $Secs
if (!$p.HasExited) { Stop-Process -Id $p.Id -Force }

$txt = Get-Content $log -Raw -Encoding UTF8
$pass1 = $txt -match "EFI stub: Loaded initrd from command line option"
$pass2 = $txt -match "SR: modules loaded"
Write-Host ("EFI stub loaded initrd : " + $pass1)
Write-Host ("rescue init ran        : " + $pass2)
if ($pass1 -and $pass2) { Write-Host "UEFI SMOKE: PASS"; exit 0 }
Write-Host "UEFI SMOKE: FAIL (log: $log)"; exit 1
