# Presence logger: records exactly when the board is on the network and how
# long each boot lives. No writes, no OTA вЂ” only /api/status every 5 s, which
# also resets the deep-sleep idle timer while the board is reachable.
param(
  [string]$Board = "leaf.local",
  [int]$Minutes = 30,
  [string]$Log = "$env:TEMP\leaf-presence.log"
)

function Log([string]$m) {
  $line = "{0:HH:mm:ss} {1}" -f (Get-Date), $m
  Write-Output $line
  Add-Content -Path $Log -Value $line -Encoding UTF8
}

$deadline = (Get-Date).AddMinutes($Minutes)
$up = $false
$lastUptime = -1
$since = Get-Date
Log "watch $Board for $Minutes min"

while ((Get-Date) -lt $deadline) {
  $s = $null
  try { $s = Invoke-RestMethod -Uri "http://$Board/api/status" -TimeoutSec 5 } catch { }

  if ($s) {
    if (-not $up) {
      $up = $true
      $since = Get-Date
      Log ("ONLINE fw={0} up={1}s reset={2} sw={3} rssi={4}" -f $s.fw, $s.uptime, $s.reset_reason, $s.software_restart, $s.wifi_rssi)
    } elseif ($s.uptime -lt $lastUptime) {
      Log ("REBOOTED up={0}s reset={1} sw={2}" -f $s.uptime, $s.reset_reason, $s.software_restart)
      $since = Get-Date
    }
    $lastUptime = $s.uptime
  } elseif ($up) {
    $up = $false
    Log ("OFFLINE after {0:N0}s uptime={1}s" -f ((Get-Date) - $since).TotalSeconds, $lastUptime)
  }

  Start-Sleep -Seconds 5
}
Log "watch finished"
