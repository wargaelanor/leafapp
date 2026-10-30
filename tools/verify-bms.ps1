# End-to-end check of the BMS/VIN work on a live car.
#
# Run this when the car can be woken (it sends the climate command, which takes
# a few kWh but wakes CAR-CAN and the diagnostic gateway):
#
#   powershell -File tools\verify-bms.ps1 -Minutes 6
#
# What it proves, field by field:
#   bmsreq / bmsq   the rotation really visits 0x02, 0x04, 0x01, 0x61
#   bmsr / bmsgrp   replies keep arriving (not just the first two after wake)
#   bmsage          seconds since the last accepted reply (should stay small)
#   bmscd           cooldown after the gateway went silent
#   btemp/cdelta/v  temperature, cell delta, voltage and current together
#   vin             result of the read-only OBD-II probe (may stay empty)
param(
  [string]$Board = "192.168.70.100",
  [int]$Minutes = 6,
  [int]$IntervalSec = 10,
  [double]$Setpoint = 22,
  [switch]$SkipClimate,
  [switch]$SkipVinProbe,
  [string]$Log = "$env:TEMP\leaf-verify-bms.log"
)

function Log([string]$m) {
  $line = "{0:HH:mm:ss} {1}" -f (Get-Date), $m
  Write-Output $line
  Add-Content -Path $Log -Value $line -Encoding UTF8
}

$status = $null
for ($i = 0; $i -lt 10 -and -not $status; $i++) {
  try { $status = Invoke-RestMethod -Uri "http://$Board/api/status" -TimeoutSec 6 } catch { Start-Sleep -Seconds 6 }
}
if (-not $status) { Log "board not reachable"; exit 2 }
Log ("board fw={0} up={1}s rssi={2}" -f $status.fw, $status.uptime, $status.wifi_rssi)

if (-not $SkipVinProbe) {
  try {
    $null = Invoke-RestMethod -Uri "http://$Board/api/developer/login" -Method POST `
      -Body @{ password = "1985" } -SessionVariable dev -TimeoutSec 10
    $probe = Invoke-RestMethod -Uri "http://$Board/api/developer/vin_probe" -Method POST `
      -WebSession $dev -TimeoutSec 10
    Log ("vin probe started: {0}" -f ($probe | ConvertTo-Json -Compress))
  } catch {
    Log ("vin probe failed: " + $_.Exception.Message)
  }
}

if (-not $SkipClimate) {
  $code = curl.exe -sS -o NUL -w "%{http_code}" -X POST `
    "http://$Board/hvac_on?setpoint=$Setpoint&minutes=10" --max-time 25
  Log ("climate command http={0} (wakes CAR-CAN and the diagnostic gateway)" -f $code)
}

$deadline = (Get-Date).AddMinutes($Minutes)
$lastAge = -1
$replies = @{}
while ((Get-Date) -lt $deadline) {
  Start-Sleep -Seconds $IntervalSec
  try { $s = Invoke-RestMethod -Uri "http://$Board/api/status" -TimeoutSec 6 }
  catch { Log "no answer"; continue }
  Log ("up={0} hvac={1} q={2} r={3} grp={4} req={5} age={6} cd={7} fail={8} bt={9} cd_mv={10} v={11} i={12} p={13} vin='{14}'" -f `
    $s.uptime, $s.hvac, $s.bmsq, $s.bmsr, $s.bmsgrp, $s.bmsreq, $s.bmsage, $s.bmscd, $s.bmsfail, `
    $s.btemp, $s.cdelta, $s.v, $s.i, $s.power, $s.vin)
  if ($s.bmsr -gt 0) { $replies[$s.bmsr] = $true }
  $lastAge = $s.bmsage
}

$summary = "summary: distinct reply counters={0}, last reply age={1}s" -f $replies.Count, $lastAge
if ($replies.Count -le 1) {
  $summary += " -> WARN: the LBC answered only once, cooldown/rate limit still applies"
} elseif ($lastAge -gt 300) {
  $summary += " -> WARN: no fresh BMS reply for more than 5 minutes"
} else {
  $summary += " -> OK: BMS replies kept coming"
}
Log $summary
