# Generates webapp_html.h from the PWA (index.html + vehicle picture) for ESP32 PROGMEM.
# Usage: powershell -File tools/mkwebapp.ps1 [-Out <path\to\webapp_html.h>]
param([string]$Out)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

# PWA location: repo root (mirror layout) or <root>/leafapp (dev tree layout)
$app = if (Test-Path (Join-Path $root 'index.html')) { $root } else { Join-Path $root 'leafapp' }
if (-not $Out) { $Out = Join-Path $root 'firmware\LeafControl\webapp_html.h' }

$html = [System.IO.File]::ReadAllText((Join-Path $app 'index.html'), [System.Text.Encoding]::UTF8)
if ($html -match '\)"rawliteral') { throw 'index.html contains rawliteral terminator - change delimiter' }
# Keep the source readable, but omit indentation and empty lines from the
# embedded copy. On the SIM800 build every saved byte matters near the OTA
# partition limit; this transformation does not alter HTML/JS token spacing.
$html = [regex]::Replace($html, '(?m)^[ \t]+', '')
$html = [regex]::Replace($html, '(?m)^\r?\n', '')

# Vehicle picture: prefer WebP (16.9 KB) over the source PNG (23.5 KB).
$imgFile = 'leaf.png'; $imgType = 'image/png'
if (-not (Test-Path (Join-Path $app $imgFile))) { $imgFile = 'leaf.png'; $imgType = 'image/png' }
$img = [System.IO.File]::ReadAllBytes((Join-Path $app $imgFile))
$hex = for ($i = 0; $i -lt $img.Length; $i++) {
  if ($i % 16 -eq 0) { "`n  " } else { ' ' }
  '0x{0:x2},' -f $img[$i]
}

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('// GENERATED FILE - do not edit. Run tools/mkwebapp.ps1')
[void]$sb.AppendLine('#ifndef WEBAPP_HTML_H')
[void]$sb.AppendLine('#define WEBAPP_HTML_H')
[void]$sb.AppendLine('#include <Arduino.h>')
[void]$sb.AppendLine()
[void]$sb.AppendLine('static const char INDEX_HTML[] PROGMEM = R"rawliteral(')
[void]$sb.Append($html)
[void]$sb.AppendLine(')rawliteral";')
[void]$sb.AppendLine()
[void]$sb.AppendLine('static const unsigned char LEAF_IMG[] PROGMEM = {' + ($hex -join '') + '};')
[void]$sb.AppendLine(('static const size_t LEAF_IMG_LEN = {0};' -f $img.Length))
[void]$sb.AppendLine(('static const char LEAF_IMG_TYPE[] PROGMEM = "{0}";' -f $imgType))
[void]$sb.AppendLine()

# Optional status-picture overlays, served from the board itself so the UI works
# without any external files. Missing files are simply skipped.
$overlays = @('car-lights.png', 'car-cable.png', 'car-wheel1.png', 'car-wheel2.png', 'car-wheel3.png', 'car-wheel4.png')
$entries = @()
foreach ($name in $overlays) {
    $file = Join-Path $app $name
    if (-not (Test-Path $file)) { continue }
    $bytes = [System.IO.File]::ReadAllBytes($file)
    $var = ($name -replace '[^A-Za-z0-9]', '_').ToUpper()
    $h = for ($i = 0; $i -lt $bytes.Length; $i++) {
        if ($i % 16 -eq 0) { "`n  " } else { ' ' }
        '0x{0:x2},' -f $bytes[$i]
    }
    [void]$sb.AppendLine(('static const unsigned char {0}[] PROGMEM = {{{1}}};' -f $var, ($h -join '')))
    $entries += ('  {{"/{0}", {1}, sizeof({1}), "image/png"}},' -f $name, $var)
    Write-Host ("  overlay {0}: {1} bytes" -f $name, $bytes.Length)
}
[void]$sb.AppendLine('struct WebAppImage { const char* path; const unsigned char* data; size_t len; const char* type; };')
if ($entries.Count -gt 0) {
    [void]$sb.AppendLine('static const WebAppImage WEBAPP_IMAGES[] = {')
    foreach ($e in $entries) { [void]$sb.AppendLine($e) }
    [void]$sb.AppendLine('};')
    [void]$sb.AppendLine(('static const size_t WEBAPP_IMAGE_COUNT = {0};' -f $entries.Count))
} else {
    [void]$sb.AppendLine('static const WebAppImage WEBAPP_IMAGES[] = {{"/none", nullptr, 0, "text/plain"}};')
    [void]$sb.AppendLine('static const size_t WEBAPP_IMAGE_COUNT = 0;')
}
[void]$sb.AppendLine()
[void]$sb.AppendLine('#endif')

[System.IO.File]::WriteAllText($Out, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
Write-Host ("Wrote {0} ({1} bytes html, {2} bytes {3})" -f $Out, $html.Length, $img.Length, $imgFile)
