# uefi-shim-smoke.ps1 - Verify the shim hop: shim loads OUR UKI from its own dir.
#
# Secure Boot OFF in this test, so shim skips verification and just passes
# through to grubx64.efi (our signed UKI) in the same directory. That validates
# the risky part we can test without MS keys enrolled in OVMF:
#   * shim's directory-relative second stage lookup (it is hard-coded to
#     grubx64.efi next to shimx64.efi)
#   * our UKI booting when launched through shim
# What it does NOT cover: real Secure Boot verification against the MOK list and
# the one-time MokManager enrollment -- that needs a Secure Boot machine.
#
# KEEP THIS FILE PURE ASCII (AGENTS.md section 15).
#
# Usage: powershell -File uefi-shim-smoke.ps1 [-Secs 150]

param([int]$Secs = 150)

$ErrorActionPreference = "Stop"
$qemu = "D:\Prog\ProgIDE\msys64\mingw64\bin\qemu-system-x86_64.exe"
$ovmf = "D:\Prog\ProgIDE\msys64\mingw64\share\qemu\edk2-x86_64-code.fd"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$sb = Join-Path $root "dist\bootfiles\sb"
$tmp = Join-Path $env:TEMP "zj-uefi-shim"

foreach ($f in @("shimx64.efi", "mmx64.efi", "zjrestore-uki.efi", "zj-mok.cer")) {
    if (!(Test-Path (Join-Path $sb $f))) {
        Write-Host "missing: $f (run make package)"; exit 1
    }
}

Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path (Join-Path $tmp "EFI\ZJRESTORE") | Out-Null
Copy-Item (Join-Path $sb "shimx64.efi") (Join-Path $tmp "EFI\ZJRESTORE")
Copy-Item (Join-Path $sb "mmx64.efi") (Join-Path $tmp "EFI\ZJRESTORE")
Copy-Item (Join-Path $sb "zjrestore-uki.efi") (Join-Path $tmp "EFI\ZJRESTORE\grubx64.efi")
Copy-Item (Join-Path $sb "zj-mok.cer") (Join-Path $tmp "EFI\ZJRESTORE")
Set-Content -Encoding ASCII (Join-Path $tmp "startup.nsh") `
    "fs0:\EFI\ZJRESTORE\shimx64.efi`r`n"

$log = Join-Path $env:TEMP "zj-uefi-shim.log"
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
$ok = $txt.Contains("Loaded initrd from LINUX_EFI_INITRD_MEDIA_GUID")
Write-Host ("shim -> our UKI -> kernel initrd : {0}" -f $ok)
Write-Host ("log                             : $log")
if ($ok) { Write-Host "UEFI SHIM SMOKE: PASS"; exit 0 }
Write-Host "UEFI SHIM SMOKE: FAIL"
exit 1
