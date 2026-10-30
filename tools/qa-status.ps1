# QA health check for the Leaf Control board.
# Usage: powershell -File tools/qa-status.ps1 [-Host leaf.local] [-Samples 3]
param(
  [string]$Board = "192.168.70.103",
  [int]$Samples = 3,
  [int]$IntervalSec = 5
)

$ErrorActionPreference = "Stop"
$base = "http://$Board"
$pass = 0
$fail = 0
$warn = 0

function Report([string]$name, [bool]$ok, [string]$detail, [bool]$warning = $false) {
  if ($ok) { $script:pass++; Write-Output ("PASS  {0,-24} {1}" -f $name, $detail) }
  elseif ($warning) { $script:warn++; Write-Output ("WARN  {0,-24} {1}" -f $name, $detail) }
  else { $script:fail++; Write-Output ("FAIL  {0,-24} {1}" -f $name, $detail) }
}

$samples = @()
for ($i = 1; $i -le $Samples; $i++) {
  try {
    $s = Invoke-RestMethod -Uri "$base/api/status" -TimeoutSec 12
    $samples += $s
    Write-Output ("sample {0}: fa={1} fb={2} evclk={3} lock={4} cdelta={5} btemp={6}" -f `
      $i, $s.fa, $s.fb, $s.evclk, $s.evlock, $s.cdelta, $s.btemp)
  } catch {
    Report "http" $false ("no answer: " + $_.Exception.Message)
    exit 1
  }
  if ($i -lt $Samples) { Start-Sleep -Seconds $IntervalSec }
}

$a = $samples[0]
$z = $samples[$samples.Count - 1]

Write-Output "---- checks ----"
Report "firmware"     ([bool]$a.fw) $a.fw
Report "heap"         ($a.free_heap -gt 60000) ("free_heap=" + $a.free_heap)
Report "mcp2515"      ($a.evcan -eq 1 -and $a.evchip -eq 1 -and $a.evcfg -eq 1) `
                      ("evcan=" + $a.evcan + " chip=" + $a.evchip + " cfg=" + $a.evcfg)
Report "ev_timing"    ($a.evclk -eq "16MHz" -and $a.evkbps -eq "500k") `
                      ($a.evclk + "/" + $a.evkbps)

$faDelta = [int]$z.fa - [int]$a.fa
$fbDelta = [int]$z.fb - [int]$a.fb
Report "portA_frames" ($faDelta -gt 0) ("delta=" + $faDelta)
Report "portB_frames" ($fbDelta -gt 0) ("delta=" + $fbDelta)

$evFresh = ($z.fage -ne 4294967295 -and $z.fage -lt 60)
Report "ev_fresh" $evFresh ("fage=" + $z.fage)

if ($evFresh) {
  Report "pack_voltage" ([double]$z.v -gt 200 -and [double]$z.v -lt 500) ("v=" + $z.v)
  Report "pack_current" ($z.pack -eq 1) ("i=" + $z.i + " A, p=" + $z.power + " kW")
} else {
  Report "pack_values" $false ("EV-CAN silent, v/i must show as unknown (fage=" + $z.fage + ")") $true
}

Report "bms_requests" ($z.bmsq -gt 0) ("bmsq=" + $z.bmsq + " bmsr=" + $z.bmsr)
Report "bms_reply"    ($z.bmsr -gt 0) ("len=" + $z.bmslen + " grp=" + $z.bmsgrp)
Report "cell_delta"   ([int]$z.cdelta -ge 0) ("cdelta=" + $z.cdelta + " mV")
Report "batt_temp"    ([double]$z.btemp -gt -30 -and [double]$z.btemp -lt 70) ("btemp=" + $z.btemp + " C")

$vin = [string]$z.vin
Report "vin" ($vin.Length -eq 17 -and $vin -match '^[A-HJ-NPR-Z0-9]{17}$') ("vin='" + $vin + "'")
Report "soc" ([double]$z.soc -gt 0 -and [double]$z.soc -le 100) ("soc=" + $z.soc)
Report "soh" ([int]$z.soh -gt 0 -and [int]$z.soh -le 100) ("soh=" + $z.soh)
Report "gids" ([int]$z.gids -gt 0 -and [int]$z.gids -le 500) ("gids=" + $z.gids)
Report "odometer" ([int]$z.odo -gt 0) ("odo=" + $z.odo + " km")
Report "twai" ($z.twai -eq 1) ("twai=" + $z.twai + " btec=" + $z.btec + " brec=" + $z.brec)
Report "ovms_v2" ($z.ovms_v2_connected -eq 1) `
                 ("connected=" + $z.ovms_v2_connected + " paranoid=" + $z.ovms_v2_paranoid) $true

Write-Output "---- raw ----"
Write-Output ("r1db={0} | r5bc={1} | r5c0={2} | r7bb={3} | r79a={4} | r1dba={5}" -f `
  $z.r1db, $z.r5bc, $z.r5c0, $z.r7bb, $z.r79a, $z.r1dba)

Write-Output ("---- result: {0} pass, {1} warn, {2} fail ----" -f $pass, $warn, $fail)
if ($fail -gt 0) { exit 2 }
exit 0
