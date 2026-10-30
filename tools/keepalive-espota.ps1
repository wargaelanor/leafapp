# Keep-alive + network (espota) flash.
#
# Two problems this solves:
#  1. The board deep-sleeps when the car has been idle past "sleep after"; GET
#     /api/status resets idleSec, so a 15 s poll holds it awake.
#  2. HTTP multipart upload of 1.4 MB dies at ~-91 dBm ("Empty reply"). The
#     espota protocol (ArduinoOTA, already enabled in this firmware) transfers
#     in acknowledged blocks and reports real errors, so it survives the link.

param(
  [string]$Board = "leaf.local",
  [string]$Firmware = "$env:TEMP\leafbin-ze1-cmd53\LeafControl.ino.bin",
  [string]$ExpectedVersion = "FW-2026.10.29.53-CMD",
  [int]$Minutes = 720,
  [string]$Log = "$env:TEMP\leaf-espota.log"
)

$espota = "$env:LOCALAPPDATA\Arduino15\packages\esp32\hardware\esp32\3.3.8\tools\espota.exe"

function Log([string]$m) {
  $line = "{0:HH:mm:ss} {1}" -f (Get-Date), $m
  Write-Output $line
  Add-Content -Path $Log -Value $line -Encoding UTF8
}

function GetStatus([int]$timeout = 8) {
  try { return Invoke-RestMethod -Uri "http://$Board/api/status" -TimeoutSec $timeout } catch { return $null }
}

Log "start board=$Board target=$ExpectedVersion"
$deadline = (Get-Date).AddMinutes($Minutes)
$seen = $false
$attempt = 0

while ((Get-Date) -lt $deadline) {
  $s = GetStatus
  if (-not $s) {
    if ($seen) { Log "board silent"; $seen = $false }
    Start-Sleep -Seconds 10
    continue
  }

  if (-not $seen) {
    Log ("AWAKE fw={0} up={1}s reset={2} rssi={3} lmax={4}" -f $s.fw, $s.uptime, $s.reset_reason, $s.wifi_rssi, $s.lmax)
    $seen = $true
  }

  if ($s.fw -eq $ExpectedVersion) {
    Log "target already installed"
    exit 0
  }

  $attempt++
  Log ("espota attempt {0}" -f $attempt)
  & $espota -i $Board -p 3232 -f $Firmware -r -t 60 2>&1 | ForEach-Object { Log ("  " + $_) }
  $code = $LASTEXITCODE
  Log "espota exit=$code"

  if ($code -eq 0) {
    for ($i = 0; $i -lt 30; $i++) {
      Start-Sleep -Seconds 10
      $v = GetStatus
      if ($v -and $v.fw -eq $ExpectedVersion) {
        Log ("INSTALLED fw={0} up={1}s" -f $v.fw, $v.uptime)
        exit 0
      }
    }
    Log "espota ok but new build not visible"
  }

  Start-Sleep -Seconds 15
}

Log "deadline reached, target not installed"
exit 3
