# mon.ps1 - send one HMP command to the QEMU monitor (ASCII only, PS 5.1)
# usage: powershell -File mon.ps1 -Cmd "screendump base/s3s.png"
param(
    [Parameter(Mandatory = $true)][string]$Cmd,
    [int]$Port = 55555,
    [int]$Ms = 900
)
$c = New-Object Net.Sockets.TcpClient
try { $c.Connect('127.0.0.1', $Port) }
catch { Write-Output "MON-CONNECT-FAIL $($_.Exception.Message)"; exit 1 }
$s = $c.GetStream()
$buf = New-Object byte[] 8192
Start-Sleep -Milliseconds 350
while ($s.DataAvailable) { [void]$s.Read($buf, 0, $buf.Length) }
$line = [Text.Encoding]::ASCII.GetBytes($Cmd + "`n")
$s.Write($line, 0, $line.Length); $s.Flush()
Start-Sleep -Milliseconds $Ms
$resp = ''
while ($s.DataAvailable) { $n = $s.Read($buf, 0, $buf.Length); $resp += [Text.Encoding]::ASCII.GetString($buf, 0, $n) }
$c.Close()
Write-Output ("MON [{0}] -> {1}" -f $Cmd, (($resp -replace '\s+', ' ').Trim()))
exit 0
