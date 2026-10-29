param(
    [string]$Pwd = 'zjtest',
    [int]$Secs = 240,
    [string]$Tag = 'enc'
)
$ErrorActionPreference = 'Continue'
$qemu = "D:\Prog\ProgIDE\msys64\mingw64\bin\qemu-system-x86_64.exe"
$wd = 'D:\Prog\_Project\SysRecover\tools\vmtest'
$log = Join-Path $wd ("logs\drill-" + $Tag + "-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + ".log")
New-Item -ItemType Directory -Force (Split-Path -Parent $log) | Out-Null
$pwdFile = Join-Path (Split-Path -Parent $log) ("pwd-" + $Tag + ".txt")
Set-Content -Path $pwdFile -Value $Pwd -Encoding ASCII -NoNewline
Add-Content -Path $pwdFile -Value "`n" -Encoding ASCII -NoNewline
$p = Start-Process -FilePath $qemu -WorkingDirectory $wd -ArgumentList @(
    "-accel", "tcg,thread=multi", "-cpu", "max", "-m", "1024",
    "-drive", "file=base/drill.raw,format=raw,if=ide",
    "-boot", "c", "-no-reboot", "-nographic"
) -RedirectStandardOutput $log -RedirectStandardError ($log + ".err") `
  -RedirectStandardInput $pwdFile -PassThru -NoNewWindow
Write-Output "qemu pid=$($p.Id) log=$log"
$p | Wait-Process -Timeout $Secs -ErrorAction SilentlyContinue
if (-not $p.HasExited) { Write-Output "timeout -> killing"; $p.Kill() } else { Write-Output "qemu exited code=$($p.ExitCode)" }
$txt = Get-Content $log -Raw -ErrorAction SilentlyContinue
foreach ($k in @('encrypted image detected','encrypted image unlocked','unlock','dmsetup','esp restored','RESTORE DONE','Restarting system','apply','ERROR')) {
    $n = ([regex]::Matches($txt, [regex]::Escape($k))).Count
    if ($n -gt 0) { Write-Output ("HIT {0,-28} x{1}" -f $k, $n) }
}
Write-Output "---- tail ----"
($txt -split "`n" | Select-Object -Last 25) | ForEach-Object { $_.TrimEnd() }
