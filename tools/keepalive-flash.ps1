# Keep-alive OTA.
#
# The board deep-sleeps when the car has been idle longer than "sleep after"
# hours, and it only wakes on a timer (default 10 min) for about 90 s.  That is
# why it "appears and disappears" and why any long upload dies: the upload
# outlives the wake window.
#
# GET /api/status calls noteUserActivity() -> idleSec = 0, so polling it holds
# the board awake.  This script polls every 15 s, and the moment the board
# answers it starts an unthrottled upload (throttling made the transfer longer
# than the wake window, which is what killed every earlier attempt).

param(
  [string]$Board = "192.168.70.100",
  [string]$Firmware = "$env:TEMP\leafbin-ze1-cmd53\LeafControl.ino.bin",
  [string]$ExpectedVersion = "FW-2026.10.29.53-CMD",
  [int]$Minutes = 720,
  [string]$Log = "$env:TEMP\leaf-keepalive.log"
)

function Log([string]$m) {
  $line = "{0:HH:mm:ss} {1}" -f (Get-Date), $m
  Write-Output $line
  Add-Content -Path $Log -Value $line -Encoding UTF8
}

function GetStatus([int]$timeout = 8) {
  try { return Invoke-RestMethod -Uri "http://$Board/api/status" -TimeoutSec $timeout } catch { return $null }
}

$deadline = (Get-Date).AddMinutes($Minutes)
Log "start board=$Board target=$ExpectedVersion deadline=$deadline"

$seen = $false
$attempts = 0

while ((Get-Date) -lt $deadline) {
  $s = GetStatus
  if (-not $s) {
    if ($seen) { Log "board gone (deep sleep / wake window ended)"; $seen = $false }
    Start-Sleep -Seconds 15
    continue
  }

  if (-not $seen) {
    Log ("AWAKE fw={0} up={1}s reset={2} sw={3} rssi={4} lmax={5}" -f `
      $s.fw, $s.uptime, $s.reset_reason, $s.software_restart, $s.wifi_rssi, $s.lmax)
    $seen = $true
  }

  if ($s.fw -eq $ExpectedVersion) {
    Log "target already installed"
    exit 0
  }

  $attempts++
  Log ("upload attempt {0} (unthrottled, up={1}s)" -f $attempts, $s.uptime)
  curl.exe -fsS --connect-timeout 20 --max-time 600 `
    -F "firmware=@$Firmware;type=application/octet-stream" "http://$Board/ota_update" | Out-Null
  $code = $LASTEXITCODE
  Log "curl exit=$code"

  if ($code -eq 0) {
    for ($i = 0; $i -lt 24; $i++) {
      Start-Sleep -Seconds 10
      $v = GetStatus
      if ($v -and $v.fw -eq $ExpectedVersion) {
        Log ("INSTALLED fw={0} up={1}s" -f $v.fw, $v.uptime)
        exit 0
      }
    }
    Log "upload reported ok but new build not visible yet"
  }

  # Board may have slept during the attempt: keep polling/keeping alive.
  $seen = $false
  Start-Sleep -Seconds 20
}

Log "deadline reached, target not installed"
exit 3
