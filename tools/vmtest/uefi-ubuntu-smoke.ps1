# uefi-ubuntu-smoke.ps1 - Validate the "variant D" rescue chain (Secure Boot OFF).
#
# Chain: OVMF Shell -> shimx64.efi -> grubx64.efi (Canonical-signed Ubuntu GRUB)
#        -> grub.cfg -> Canonical-signed Ubuntu kernel + OUR initramfs
#        -> our /init (Alpine userland + Ubuntu modules) runs.
#
# What this proves WITHOUT a Secure Boot environment: the Ubuntu kernel boots our
# initramfs and the rescue layer comes up (module loading works, /init runs).
# The one thing only a real SB machine can prove is GRUB accepting the kernel's
# Canonical signature -- and that is already established by Ubuntu's own design.
#
# The test writes its own grub.cfg with `console=ttyS0` so the rescue output is
# visible on the serial log (the shipped grub.cfg uses console=tty0).
#
# KEEP THIS FILE PURE ASCII (AGENTS.md section 15).
#
# Usage: powershell -File uefi-ubuntu-smoke.ps1 [-Secs 300]

param([int]$Secs = 300)

$ErrorActionPreference = "Stop"
$qemu = "D:\Prog\ProgIDE\msys64\mingw64\bin\qemu-system-x86_64.exe"
$ovmf = "D:\Prog\ProgIDE\msys64\mingw64\share\qemu\edk2-x86_64-code.fd"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
# shim/mm belong to the parked "MOK/UKI" line: they are no longer shipped in
# dist/, but they still live in the repo (bootfiles/sb/) and this chain needs
# them, so take the assets from the repo.
$sb = Join-Path $root "bootfiles\sb"
$boot = Join-Path $root "bootfiles"
$tmp = Join-Path $env:TEMP "zj-uefi-ubuntu"

foreach ($f in @("shimx64.efi", "mmx64.efi", "grub-ubuntu.efi")) {
    if (!(Test-Path (Join-Path $sb $f))) {
        Write-Host "missing $f (run make package)"; exit 1
    }
}

Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path (Join-Path $tmp "EFI\ZJRESTORE") | Out-Null
$d = Join-Path $tmp "EFI\ZJRESTORE"
Copy-Item (Join-Path $sb "shimx64.efi") $d
Copy-Item (Join-Path $sb "mmx64.efi") $d
Copy-Item (Join-Path $sb "grub-ubuntu.efi") (Join-Path $d "grubx64.efi")
Copy-Item (Join-Path $boot "vmlinuz-zjrestore") $d
Copy-Item (Join-Path $boot "initramfs-zjrestore.cpio.gz") $d
$cfg = @"
set timeout=0
menuentry 'SysRecover' {
    linux /EFI/ZJRESTORE/vmlinuz-zjrestore console=ttyS0 nvme_core.io_timeout=1 zjre=1
    initrd /EFI/ZJRESTORE/initramfs-zjrestore.cpio.gz
}
"@
Set-Content -Encoding ASCII (Join-Path $d "grub.cfg") $cfg
Set-Content -Encoding ASCII (Join-Path $tmp "startup.nsh") `
    "fs0:\EFI\ZJRESTORE\shimx64.efi`r`n"

$log = Join-Path $env:TEMP "zj-uefi-ubuntu.log"
Remove-Item $log -Force -ErrorAction SilentlyContinue
$qargs = @("-m","2048","-smp","2",
           "-drive","if=pflash,format=raw,unit=0,readonly=on,file=$ovmf",
           "-drive","file=fat:rw:$tmp,format=raw,if=ide,index=0",
           "-nographic","-no-reboot")
$p = Start-Process -FilePath $qemu -ArgumentList $qargs -RedirectStandardOutput $log -NoNewWindow -PassThru
Start-Sleep -Seconds $Secs
if (!$p.HasExited) { Stop-Process -Id $p.Id -Force }

Start-Sleep -Seconds 2
$txt = Get-Content $log -Raw -Encoding UTF8
$pass1 = $txt.Contains("SR: modules loaded")
$pass2 = $txt.Contains("restore")
$pass3 = $txt.Contains("Linux version 6.8.0-31-generic")
Write-Host ("ubuntu kernel booted    : {0}" -f $pass3)
Write-Host ("rescue init ran         : {0}" -f $pass1)
Write-Host ("log                     : $log")
if ($pass1) { Write-Host "UEFI UBUNTU SMOKE: PASS"; exit 0 }
Write-Host "UEFI UBUNTU SMOKE: FAIL"
exit 1
