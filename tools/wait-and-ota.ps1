param(
    [string]$Device = "http://192.168.70.104",
    [string]$Firmware = "$env:TEMP\leafbin-ze1-cmd51\LeafControl.ino.bin",
    [string]$ExpectedVersion = "FW-2026.10.29.51-CMD",
    [int]$PollSeconds = 3,
    [int]$WaitMinutes = 30
)

$ErrorActionPreference = "Stop"
$Device = $Device.TrimEnd("/")

if (-not (Test-Path -LiteralPath $Firmware -PathType Leaf)) {
    throw "Firmware not found: $Firmware"
}

$firmwareFile = Get-Item -LiteralPath $Firmware
if ($firmwareFile.Length -lt 100000 -or $firmwareFile.Length -gt 3145728) {
    throw "Unexpected firmware size: $($firmwareFile.Length) bytes"
}

function Get-LeafStatus {
    # Windows PowerShell promotes native stderr to an ErrorRecord when the
    # global preference is Stop. A connection timeout is expected while the
    # board boots, so suppress it only for this probe.
    $savedErrorPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "SilentlyContinue"
        $text = & curl.exe -fsS --connect-timeout 2 --max-time 5 `
            "$Device/api/status" 2>$null
        $curlExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedErrorPreference
    }

    if ($curlExitCode -ne 0 -or [string]::IsNullOrWhiteSpace($text)) {
        return $null
    }

    try {
        $status = $text | ConvertFrom-Json
    } catch {
        return $null
    }

    if (-not $status.fw -or -not ([string]$status.fw).StartsWith("FW-")) {
        return $null
    }
    return $status
}

Write-Host "Firmware: $($firmwareFile.FullName) ($($firmwareFile.Length) bytes)"
Write-Host "Waiting for Leaf Control at $Device ..."

$deadline = (Get-Date).AddMinutes($WaitMinutes)
$status = $null
while ((Get-Date) -lt $deadline) {
    $status = Get-LeafStatus
    if ($null -ne $status) {
        Write-Host "Device found. Current firmware: $($status.fw)"
        break
    }
    Start-Sleep -Seconds $PollSeconds
}

if ($null -eq $status) {
    throw "Device did not become ready within $WaitMinutes minutes"
}

if ([string]$status.fw -eq $ExpectedVersion) {
    Write-Host "Expected firmware is already installed. Nothing to do."
    exit 0
}

# Upload once. A weak in-car Wi-Fi link dropped an unrestricted multipart
# stream, while 64 KiB/s completed reliably. Never retry an upload blindly.
Write-Host "Uploading $ExpectedVersion ..."
& curl.exe -fsS --connect-timeout 10 --max-time 360 --limit-rate 64k `
    -F "firmware=@$($firmwareFile.FullName);type=application/octet-stream" `
    "$Device/ota_update"
$uploadExitCode = $LASTEXITCODE

if ($uploadExitCode -eq 0) {
    Write-Host "Upload accepted. Waiting for reboot and version confirmation ..."
} else {
    # The board may reboot after committing the image before curl receives the
    # final response. Verify the reported version before declaring failure.
    Write-Host "Upload connection ended with curl code $uploadExitCode; checking device version ..."
}
$verifyDeadline = (Get-Date).AddMinutes(3)
$confirmed = $false
while ((Get-Date) -lt $verifyDeadline) {
    Start-Sleep -Seconds $PollSeconds
    $status = Get-LeafStatus
    if ($null -ne $status) {
        Write-Host "Device reports: $($status.fw)"
        if ([string]$status.fw -eq $ExpectedVersion) {
            $confirmed = $true
            break
        }
    }
}

if (-not $confirmed) {
    throw "OTA was not confirmed after reboot (curl exit code $uploadExitCode)"
}

Write-Host "OTA complete: $ExpectedVersion"
