# run-dual-qemu.ps1 - A/B swap E2E test QEMU launcher (ASCII only, PS 5.1)
#
# Start/wait/kill one QEMU run.  Start is a separate short call (-Detach) so a
# long tool timeout can never kill the guest mid-write; poll with -WaitOnly.
# One QEMU at a time -> fixed HMP monitor TCP port (sendkey fallback if the
# guest ever sticks at a login screen).
#
# Examples:
#   .\run-dual-qemu.ps1 -Stage s1 -Kernel <vmlinuz> -Initrd <initramfs> -Detach
#   .\run-dual-qemu.ps1 -Stage s1 -WaitOnly -TimeoutSec 280
#   .\run-dual-qemu.ps1 -Stage s1 -Kill
param(
    [string]$Stage = 's1',
    [string]$Disk = 'dual.raw',
    [string]$DiskFormat = 'raw',
    [int]$Mem = 1024,
    [string]$Kernel = '',
    [string]$Initrd = '',
    [string]$Append = 'console=tty0 console=ttyS0,115200 loglevel=7',
    [int]$TimeoutSec = 280,
    [int]$MonitorPort = 55555,
    [switch]$Detach,
    [switch]$WaitOnly,
    [switch]$Kill
)

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$qemu = 'D:\Prog\ProgIDE\msys64\mingw64\bin\qemu-system-x86_64.exe'
$pidFile = Join-Path $here "base\qemu-$Stage.pid"
$outLog = Join-Path $here "base\qemu-$Stage.out.log"
$errLog = Join-Path $here "base\qemu-$Stage.err.log"

function Get-StoredPid {
    if (Test-Path $pidFile) {
        try { return [int]((Get-Content $pidFile -Raw).Trim()) } catch { return 0 }
    }
    return 0
}

if ($Kill) {
    $q = Get-StoredPid
    if ($q -gt 0) {
        $pr = Get-Process -Id $q -ErrorAction SilentlyContinue
        if ($pr) { Stop-Process -Id $q -Force -ErrorAction SilentlyContinue }
        Remove-Item $pidFile -Force -ErrorAction SilentlyContinue
        Write-Output "[$Stage] kill requested pid=$q"
    } else {
        Write-Output "[$Stage] no pid file"
    }
    exit 0
}

$proc = $null
if (-not $WaitOnly) {
    $old = Get-StoredPid
    if ($old -gt 0) {
        $pr = Get-Process -Id $old -ErrorAction SilentlyContinue
        if ($pr) {
            Write-Output "[$Stage] already running pid=$old"
            exit 1
        }
        Remove-Item $pidFile -Force -ErrorAction SilentlyContinue
    }
    $qa = @(
        '-accel', 'tcg,thread=multi', '-cpu', 'max', '-m', "$Mem",
        '-drive', "file=$Disk,format=$DiskFormat,if=ide",
        '-monitor', "tcp:127.0.0.1:$MonitorPort,server,nowait",
        '-boot', 'c', '-no-reboot', '-nographic'
    )
    if ($Kernel) {
        # PS 5.1 Start-Process joins array items with spaces and does NOT quote
        # values containing spaces -> embed our own quotes for -append.
        $qa += @('-kernel', $Kernel, '-initrd', $Initrd,
                 '-append', "`"$Append`"")
    }
    foreach ($f in @($outLog, $errLog)) {
        if (Test-Path $f) { Remove-Item $f -Force -ErrorAction SilentlyContinue }
    }
    $proc = Start-Process -FilePath $qemu -WorkingDirectory $here -ArgumentList $qa `
        -RedirectStandardOutput $outLog -RedirectStandardError $errLog `
        -WindowStyle Hidden -PassThru
    "$($proc.Id)" | Set-Content -Path $pidFile -Encoding ASCII
    $k = 'no'
    if ($Kernel) { $k = 'yes' }
    Write-Output "[$Stage] STARTED pid=$($proc.Id) disk=$Disk fmt=$DiskFormat mem=$Mem kernel=$k"
    if ($Detach) { exit 0 }
} else {
    $q = Get-StoredPid
    if ($q -le 0) {
        Write-Output "[$Stage] NO-PID-FILE"
        exit 1
    }
    try { $proc = Get-Process -Id $q -ErrorAction Stop }
    catch {
        Remove-Item $pidFile -Force -ErrorAction SilentlyContinue
        Write-Output "[$Stage] EXITED code=gone (before wait)"
        exit 0
    }
}

# ---- wait loop (proc is a live .NET Process object) ----
$start = Get-Date
$deadline = $start.AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline) {
    try {
        if ($proc.HasExited) {
            $el = [int]((Get-Date) - $start).TotalSeconds
            $code = 'unknown'
            try { $code = $proc.ExitCode } catch { $code = 'gone' }
            Remove-Item $pidFile -Force -ErrorAction SilentlyContinue
            Write-Output "[$Stage] EXITED code=$code elapsed=${el}s"
            exit 0
        }
    } catch {
        Remove-Item $pidFile -Force -ErrorAction SilentlyContinue
        Write-Output "[$Stage] EXITED code=gone elapsed"
        exit 0
    }
    Start-Sleep -Seconds 5
    $el = [int]((Get-Date) - $start).TotalSeconds
    if (($el % 60) -lt 6) {
        $tail = ''
        if (Test-Path $outLog) {
            $tail = Get-Content $outLog -Tail 1 -ErrorAction SilentlyContinue
            if ($tail) { $tail = $tail.Substring(0, [Math]::Min(110, $tail.Length)) }
        }
        Write-Output "[$Stage] running ${el}s tail=[$tail]"
    }
}
Write-Output "[$Stage] STILL-RUNNING pid=$($proc.Id) after ${TimeoutSec}s"
exit 3
