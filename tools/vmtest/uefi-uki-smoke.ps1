# uefi-uki-smoke.ps1 - Verify our signed UKI actually works (no Secure Boot needed).
#
# The UKI is built by tools/build-uki.py: systemd-stub + .cmdline/.linux/.initrd
# sections, signed with our MOK key. When the stub runs, it registers the initrd
# as a LINUX_EFI_INITRD_MEDIA_GUID device path; the kernel then prints
#   "EFI stub: Loaded initrd from LINUX_EFI_INITRD_MEDIA_GUID device path"
# on the EFI console (= serial here). That single line proves the whole UKI
# plumbing (stub ran, sections were found, initrd was handed over).
#
# KEEP THIS FILE PURE ASCII (PowerShell 5.1 parses BOM-less .ps1 as ANSI and
# CJK comments can derail the parser -- AGENTS.md section 15).
#
# Usage: powershell -File uefi-uki-smoke.ps1 [-Secs 150]

param([int]$Secs = 150)

$ErrorActionPreference = "Stop"
$qemu = "D:\Prog\ProgIDE\msys64\mingw64\bin\qemu-system-x86_64.exe"
$ovmf = "D:\Prog\ProgIDE\msys64\mingw64\share\qemu\edk2-x86_64-code.fd"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$uki = Join-Path $root "dist\bootfiles\sb\zjrestore-uki.efi"
$tmp = Join-Path $env:TEMP "zj-uefi-uki"

if (!(Test-Path $uki)) { Write-Host "UKI not found: $uki (run make package)"; exit 1 }

Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path (Join-Path $tmp "EFI\ZJRESTORE") | Out-Null
Copy-Item $uki (Join-Path $tmp "EFI\ZJRESTORE\grubx64.efi")
Set-Content -Encoding ASCII (Join-Path $tmp "startup.nsh") `
    "fs0:\EFI\ZJRESTORE\grubx64.efi`r`n"

$log = Join-Path $env:TEMP "zj-uefi-uki.log"
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
$okInitrd = $txt.Contains("Loaded initrd from LINUX_EFI_INITRD_MEDIA_GUID")
Write-Host ("UKI delivered initrd to kernel : {0}" -f $okInitrd)
Write-Host ("log                            : $log")
if ($okInitrd) { Write-Host "UEFI UKI SMOKE: PASS"; exit 0 }
Write-Host "UEFI UKI SMOKE: FAIL"
exit 1
