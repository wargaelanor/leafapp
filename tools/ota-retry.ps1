# Retry OTA upload without touching the board while the transfer runs.
#
# Measured facts (2026-10-30, -88…-92 dBm):
#  - a parallel /api/status request kills the upload within seconds
#    (single-threaded WebServer), which is what made every earlier attempt
#    fail at ~12 s;
#  - without polling the same upload reached 960 KB of 1 391 936 at 15 KB/s
#    before the link dropped, so a full transfer needs ~90 s of clean link.
# So: poll only before the transfer (this also resets the sleep idle timer),
# then let curl own the board until it finishes or fails.
param(
  [string]$Board = "192.168.70.100",
  [string]$Firmware = "$env:TEMP\leafbin-ze1-cmd53\LeafControl.ino.bin",
  [string]$ExpectedVersion = "FW-2026.10.29.53-CMD",
  [int]$Attempts = 20,
  [string]$Log = "$env:TEMP\leaf-ota-retry.log"
)

function Log([string]$m) {
  $line = "{0:HH:mm:ss} {1}" -f (Get-Date), $m
  Write-Output $line
  Add-Content -Path $Log -Value $line -Encoding UTF8
}

for ($i = 1; $i -le $Attempts; $i++) {
  $s = $null
  for ($w = 0; $w -lt 6 -and -not $s; $w++) {
    try { $s = Invoke-RestMethod -Uri "http://$Board/api/status" -TimeoutSec 6 } catch { Start-Sleep -Seconds 10 }
  }
  if (-not $s) { Log "board not reachable, waiting"; continue }

  if ($s.fw -eq $ExpectedVersion) { Log "already on $ExpectedVersion"; exit 0 }

  Log ("attempt {0}: up={1}s reset={2} rssi={3}" -f $i, $s.uptime, $s.reset_reason, $s.wifi_rssi)
  $out = curl.exe -sS --max-time 400 -w "http=%{http_code} up=%{size_upload} t=%{time_total}s" `
    -F "firmware=@$Firmware;type=application/octet-stream" "http://$Board/ota_update" 2>&1
  Log ("  " + ($out -join ' ') + " exit=$LASTEXITCODE")

  if ($LASTEXITCODE -eq 0) {
    for ($k = 0; $k -lt 20; $k++) {
      Start-Sleep -Seconds 10
      try { $v = Invoke-RestMethod -Uri "http://$Board/api/status" -TimeoutSec 8 } catch { continue }
      if ($v.fw -eq $ExpectedVersion) { Log ("INSTALLED fw={0} up={1}s" -f $v.fw, $v.uptime); exit 0 }
    }
    Log "upload returned ok but version did not change"
  }
  Start-Sleep -Seconds 5
}
Log "all attempts failed"
exit 3
