param(
    [string]$Pwd = 'zjtest',
    [int]$Secs = 300,
    [string]$Tag = 'enc'
)
$ErrorActionPreference = 'Continue'
$qemu = "D:\Prog\ProgIDE\msys64\mingw64\bin\qemu-system-x86_64.exe"
$wd = 'D:\Prog\_Project\SysRecover\tools\vmtest'
$log = Join-Path $wd ("logs\drill-" + $Tag + "-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + ".log")
New-Item -ItemType Directory -Force (Split-Path -Parent $log) | Out-Null

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $qemu
$psi.WorkingDirectory = $wd
$psi.Arguments = '-accel tcg,thread=multi -cpu max -m 1024 -drive file=base/drill.raw,format=raw,if=ide -boot c -no-reboot -nographic'
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.UseShellExecute = $false
$p = New-Object System.Diagnostics.Process
$p.StartInfo = $psi
$sb = New-Object System.Text.StringBuilder
$null = $p.Start()

$onData = {
    if ($EventArgs.Data -ne $null) { [void]$Event.MessageData.AppendLine($EventArgs.Data) }
}
$h1 = Register-ObjectEvent -InputObject $p -EventName OutputDataReceived -Action $onData -MessageData $sb
$h2 = Register-ObjectEvent -InputObject $p -EventName ErrorDataReceived -Action $onData -MessageData $sb
$p.BeginOutputReadLine()
$p.BeginErrorReadLine()

$sw = [Diagnostics.Stopwatch]::StartNew()
$sentAt = -1
$lastSent = -1
while (-not $p.HasExited -and $sw.Elapsed.TotalSeconds -lt $Secs) {
    Start-Sleep -Milliseconds 250
    $txt = $sb.ToString()
    # Wait until the rescue layer has found the image (complete line -> event fired),
    # then send the password once, 1.2s later (prompt is printed without newline).
    if ($sentAt -lt 0 -and $txt.Contains('found image:')) {
        Start-Sleep -Milliseconds 1200
        try { $p.StandardInput.Write($Pwd + "`n"); $p.StandardInput.Flush() } catch {}
        $sentAt = [int]$sw.Elapsed.TotalSeconds
        Write-Output "password sent at ${sentAt}s"
    }
    # If the MAC check complains, we got the password wrong -> retry once with a delay
    if ($sentAt -ge 0 -and $txt.Contains('wrong image password') -and
        $lastSent -lt 0 -and $sw.Elapsed.TotalSeconds -gt ($sentAt + 3)) {
        try { $p.StandardInput.Write($Pwd + "`n"); $p.StandardInput.Flush() } catch {}
        $lastSent = [int]$sw.Elapsed.TotalSeconds
        Write-Output "password resent at ${lastSent}s"
    }
}
if (-not $p.HasExited) { Write-Output "timeout -> killing"; $p.Kill() }
Start-Sleep -Milliseconds 800
Unregister-Event -SourceIdentifier $h1.Name -ErrorAction SilentlyContinue
Unregister-Event -SourceIdentifier $h2.Name -ErrorAction SilentlyContinue
$txt = $sb.ToString()
Set-Content -Path $log -Value $txt -Encoding UTF8
Write-Output "log=$log"
foreach ($k in @('password ok (header MAC','encrypted image unlocked','RESTORE DONE','Restarting system','STEP=','wrong image password','esp restored','apply ok')) {
    $n = ([regex]::Matches($txt, [regex]::Escape($k))).Count
    if ($n -gt 0) { Write-Output ("HIT {0,-30} x{1}" -f $k, $n) }
}
Write-Output "---- ZJ lines ----"
($txt -split "`n" | Select-String -Pattern 'ZJ: ' | Select-Object -Last 16 | ForEach-Object { $_.Line.Trim() })
