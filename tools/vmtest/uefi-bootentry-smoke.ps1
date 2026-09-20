# uefi-bootentry-smoke.ps1 - Validate the "method 1" hypothesis in QEMU/OVMF.
#
# Hypothesis (mature method, same as Rufus / bcfg / efibootmgr):
#   A persistent firmware boot option (Boot#### + BootOrder) can point DIRECTLY at
#   our kernel (EFI stub, subsystem 10), i.e. the firmware itself loads it -- the
#   exact step Windows Boot Manager's "bootapp" cannot do (0xc000007b).
#
# What is asserted here: the firmware LOADS and STARTS our kernel from the entry:
#       BdsDxe: loading  Boot0003 "ZJTEST" from .../vmlinuz-zjrestore.efi
#       BdsDxe: starting Boot0003 "ZJTEST" from ...
# The OptionalData -> kernel command line part is NOT covered by this script
# (the EDK2 shell's `bcfg boot -opt <idx> <data>` syntax was not cracked); that is
# established by the UEFI spec + the kernel's efi_convert_cmdline() source + the
# Rufus/efibootmgr precedent, and confirmed end-to-end on real VM/hardware.
#
# NOTE: keep this file pure ASCII. PowerShell 5.1 parses a BOM-less .ps1 as ANSI
# and CJK comments can derail the parser (AGENTS.md section 15).
#
# Usage: powershell -File uefi-bootentry-smoke.ps1 [-Secs 140]

param([int]$Secs = 140)

$ErrorActionPreference = "Stop"
$qemu = "D:\Prog\ProgIDE\msys64\mingw64\bin\qemu-system-x86_64.exe"
$share = "D:\Prog\ProgIDE\msys64\mingw64\share\qemu"
$ovmf = Join-Path $share "edk2-x86_64-code.fd"
$varsSrc = Join-Path $share "edk2-i386-vars.fd"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$boot = Join-Path $root "bootfiles"
$tmp  = Join-Path $env:TEMP "zj-uefi-entry"
$vars = Join-Path $env:TEMP "zj-uefi-vars.fd"

if (!(Test-Path $qemu)) { Write-Host "qemu not found: $qemu"; exit 1 }
if (!(Test-Path $ovmf)) { Write-Host "OVMF not found: $ovmf"; exit 1 }
if (!(Test-Path $varsSrc)) { Write-Host "vars fd not found: $varsSrc"; exit 1 }

Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path (Join-Path $tmp "EFI\ZJRESTORE") | Out-Null
Copy-Item (Join-Path $boot "vmlinuz-zjrestore") (Join-Path $tmp "EFI\ZJRESTORE\vmlinuz-zjrestore.efi")
Copy-Item (Join-Path $boot "initramfs-zjrestore.cpio.gz") (Join-Path $tmp "EFI\ZJRESTORE\initramfs-zjrestore.cpio.gz")

$nsh = @(
  "fs0:",
  "bcfg boot add 0 fs0:\EFI\ZJRESTORE\vmlinuz-zjrestore.efi `"ZJTEST`"",
  "bcfg boot dump -v",
  "reset -c"
) -join "`r`n"
Set-Content -Encoding ASCII (Join-Path $tmp "startup.nsh") ($nsh + "`r`n")

# A real NVRAM variable store, so the new Boot#### survives the reset.
Copy-Item $varsSrc $vars -Force

$log = Join-Path $env:TEMP "zj-uefi-entry.log"
Remove-Item $log -Force -ErrorAction SilentlyContinue
$qargs = @("-m","1024","-smp","2",
           "-drive","if=pflash,format=raw,unit=0,readonly=on,file=$ovmf",
           "-drive","if=pflash,format=raw,unit=1,file=$vars",
           "-drive","file=fat:rw:$tmp,format=raw,if=ide,index=0",
           "-nographic")
$p = Start-Process -FilePath $qemu -ArgumentList $qargs -RedirectStandardOutput $log -NoNewWindow -PassThru
Start-Sleep -Seconds $Secs
if (!$p.HasExited) { Stop-Process -Id $p.Id -Force }

Start-Sleep -Seconds 2
$txt = Get-Content $log -Raw -Encoding UTF8
Write-Host ("log bytes: {0}" -f $txt.Length)
$okLoad  = $txt.Contains('BdsDxe: loading Boot0003 "ZJTEST"')
$okStart = $txt.Contains('BdsDxe: starting Boot0003 "ZJTEST"')
Write-Host ("firmware loaded kernel : {0}" -f $okLoad)
Write-Host ("firmware started kernel: {0}" -f $okStart)
Write-Host ("log                    : $log")
if ($okLoad -and $okStart) { Write-Host "UEFI BOOTENTRY SMOKE: PASS"; exit 0 }
Write-Host "UEFI BOOTENTRY SMOKE: FAIL"
exit 1
