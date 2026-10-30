# Unattended OTA + capture, for when the board is behind deep sleep/weak Wi-Fi.
# Waits for a reachable window, installs the firmware, then records a short
# telemetry window the next time the car is active by itself.
param(
  [string]$Board = "192.168.70.100",
  [string]$Firmware = "$env:TEMP\leafbin-ze1-cmd52\LeafControl.ino.bin",
  [string]$ExpectedVersion = "FW-2026.10.29.52-CMD",
  [int]$Hours = 6,
  [string]$Log = "$env:TEMP\leaf-auto.log"
)

function Log([string]$m) {
  $line = "{0:HH:mm:ss} {1}" -f (Get-Date), $m
  Write-Output $line
  Add-Content -Path $Log -Value $line -Encoding UTF8
}

function GetStatus {
  try { return Invoke-RestMethod -Uri "http://$Board/api/status" -TimeoutSec 10 } catch { return $null }
}

Log "start board=$Board fw=$ExpectedVersion deadline=$((Get-Date).AddHours($Hours))"
$deadline = (Get-Date).AddHours($Hours)
$flashed = $false

while ((Get-Date) -lt $deadline) {
  $s = GetStatus
  if ($s) {
    Log ("up={0} fw={1} fa={2} fb={3} bc={4}" -f $s.uptime, $s.fw, $s.fa, $s.fb, $s.bmsr)
    if (-not $flashed -and $s.fw -ne $ExpectedVersion) {
      Log "flashing $ExpectedVersion"
      foreach ($rate in @("8k","6k","4k","2k")) {
        curl.exe -fsS --connect-timeout 25 --max-time 1800 --limit-rate $rate `
          -F "firmware=@$Firmware;type=application/octet-stream" "http://$Board/ota_update" | Out-Null
        if ($LASTEXITCODE -eq 0) { Log "upload ok at $rate"; break }
        Log "upload failed at $rate (exit $LASTEXITCODE)"
        Start-Sleep -Seconds 20
      }
      Start-Sleep -Seconds 40
      $v = GetStatus
      if ($v -and $v.fw -eq $ExpectedVersion) { $flashed = $true; Log "firmware active: $($v.fw)" }
      continue
    }
    if ($s.fw -eq $ExpectedVersion) { $flashed = $true }

    # Car active by itself? Then capture a telemetry window.
    $a = $s
    Start-Sleep -Seconds 10
    $b = GetStatus
    if ($b -and ($b.fa -gt $a.fa + 200 -or $b.fb -gt $a.fb + 200)) {
      Log "car active, capturing"
      for ($i = 0; $i -lt 24; $i++) {
        $t = GetStatus
        if ($t) {
          Log ("t={0} hvac={1} fa={2} fb={3} fage={4} v={5} i={6} cdelta={7} btemp={8} bmsq={9} bmsr={10} len={11} grp={12} vin='{13}'" -f `
            $i, $t.hvac, $t.fa, $t.fb, $t.fage, $t.v, $t.i, $t.cdelta, $t.btemp, $t.bmsq, $t.bmsr, $t.bmslen, $t.bmsgrp, $t.vin)
        }
        Start-Sleep -Seconds 8
      }
      Log "capture done"
      exit 0
    }
  }
  Start-Sleep -Seconds 45
}
Log "deadline reached without capture"
exit 3
