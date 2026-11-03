param(
  [ValidateSet("t2can", "tcall")]
  [string]$Board = "t2can"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\mkwebapp.ps1 -Board $Board | Select-Object -Last 1

  $cli = Join-Path $env:LOCALAPPDATA "Temp\opencode\arduino-cli\arduino-cli.exe"
  if (-not (Test-Path $cli)) { $cli = "arduino-cli" }
  $cfg = Join-Path $env:USERPROFILE ".arduinoIDE\arduino-cli.yaml"
  $out = Join-Path $env:TEMP "leafbin-$Board"

  if ($Board -eq "tcall") {
    $fqbn = "esp32:esp32:esp32:FlashSize=4M,PartitionScheme=min_spiffs,PSRAM=enabled"
    $extra = "-DLEAF_BOARD_TCALL=1"
  } else {
    $fqbn = "esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=disabled"
    $extra = ""
  }

  $args = @("compile", "--config-file", $cfg, "--fqbn", $fqbn, "--output-dir", $out)
  if ($extra -ne "") {
    $args += @("--build-property", "build.extra_flags=$extra")
  }
  $args += "firmware\LeafControl"

  Write-Host "=== Building $Board ==="
  Write-Host "FQBN: $fqbn"
  if ($extra -ne "") { Write-Host "Flags: $extra" }
  & $cli @args
  if ($LASTEXITCODE -ne 0) { throw "build failed with exit code $LASTEXITCODE" }

  $bin = Join-Path $out "LeafControl.ino.bin"
  if (Test-Path $bin) {
    $size = (Get-Item $bin).Length
    Write-Host ("BIN: {0} ({1:N0} bytes)" -f $bin, $size)
  }
} finally {
  Pop-Location
}
