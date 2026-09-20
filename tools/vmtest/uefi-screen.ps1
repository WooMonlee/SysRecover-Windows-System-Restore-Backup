# uefi-screen.ps1 - Verify that the rescue kernel has a WORKING on-screen console
# under UEFI (the "black screen looks like a hang" regression, PIT-061).
#
# Method: boot our kernel via the OVMF shell with `console=tty0` (VGA only, no
# serial), then take two screendumps and compare the raw bytes.
#   * identical  -> frozen frame = no framebuffer console (fbcon missing)
#   * different  -> the text console is alive (kernel/rescue is drawing)
#
# Usage: powershell -File uefi-screen.ps1 [-Secs1 30] [-Secs2 130]

param([int]$Secs1 = 30, [int]$Secs2 = 130)

$ErrorActionPreference = "Stop"
$qemu = "D:\Prog\ProgIDE\msys64\mingw64\bin\qemu-system-x86_64.exe"
$ovmf = "D:\Prog\ProgIDE\msys64\mingw64\share\qemu\edk2-x86_64-code.fd"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$boot = Join-Path $root "bootfiles"
$tmp  = Join-Path $env:TEMP "zj-uefi-screen"
$port = 5601

if (!(Test-Path $qemu)) { Write-Host "qemu not found"; exit 1 }

Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path (Join-Path $tmp "EFI\ZJRESTORE") | Out-Null
Copy-Item (Join-Path $boot "vmlinuz-zjrestore") (Join-Path $tmp "EFI\ZJRESTORE\vmlinuz-zjrestore.efi")
Copy-Item (Join-Path $boot "initramfs-zjrestore.cpio.gz") (Join-Path $tmp "EFI\ZJRESTORE\initramfs-zjrestore.cpio.gz")
$nsh = "fs0:\EFI\ZJRESTORE\vmlinuz-zjrestore.efi initrd=\EFI\ZJRESTORE\initramfs-zjrestore.cpio.gz console=tty0 nvme_core.io_timeout=1`r`n"
Set-Content -Encoding ASCII (Join-Path $tmp "startup.nsh") $nsh

function Shot([string]$path) {
    $c = New-Object System.Net.Sockets.TcpClient("127.0.0.1", $port)
    $st = $c.GetStream()
    $w = New-Object System.IO.StreamWriter($st)
    $w.NewLine = "`n"
    $w.WriteLine("screendump $path")
    $w.Flush()
    Start-Sleep -Seconds 4
    $c.Close()
}

$s1 = Join-Path $env:TEMP "zj-scr1.ppm"
$s2 = Join-Path $env:TEMP "zj-scr2.ppm"
Remove-Item $s1, $s2 -Force -ErrorAction SilentlyContinue

$qargs = @("-m","1024","-smp","2",
           "-drive","if=pflash,format=raw,unit=0,readonly=on,file=$ovmf",
           "-drive","file=fat:rw:$tmp,format=raw,if=ide,index=0",
           "-display","none",
           "-monitor","tcp:127.0.0.1:$port,server,nowait",
           "-no-reboot")
$p = Start-Process -FilePath $qemu -ArgumentList $qargs -NoNewWindow -PassThru
Start-Sleep -Seconds $Secs1
Shot $s1
Start-Sleep -Seconds ($Secs2 - $Secs1)
Shot $s2
if (!$p.HasExited) { Stop-Process -Id $p.Id -Force }

$h1 = (Get-FileHash $s1 -Algorithm MD5).Hash
$h2 = (Get-FileHash $s2 -Algorithm MD5).Hash
$l1 = (Get-Item $s1).Length
$l2 = (Get-Item $s2).Length
Write-Host "shot1: $h1 ($l1 bytes)"
Write-Host "shot2: $h2 ($l2 bytes)"
if ($h1 -ne $h2) {
    Write-Host "UEFI SCREEN: PASS (console is alive / screen changed)"
    exit 0
}
Write-Host "UEFI SCREEN: FAIL (screen frozen -> no framebuffer console?)"
exit 1
