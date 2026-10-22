# Generates webapp_html.h from the PWA (index.html + leaf.png) for ESP32 PROGMEM.
# Usage: powershell -File tools/mkwebapp.ps1 [-Out <path\to\webapp_html.h>]
param([string]$Out)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

# PWA location: repo root (mirror layout) or <root>/leafapp (dev tree layout)
$app = if (Test-Path (Join-Path $root 'index.html')) { $root } else { Join-Path $root 'leafapp' }
if (-not $Out) { $Out = Join-Path $root 'firmware\LeafControl\webapp_html.h' }

$html = [System.IO.File]::ReadAllText((Join-Path $app 'index.html'), [System.Text.Encoding]::UTF8)
if ($html -match '\)"rawliteral') { throw 'index.html contains rawliteral terminator - change delimiter' }

$img = [System.IO.File]::ReadAllBytes((Join-Path $app 'leaf.png'))
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
[void]$sb.AppendLine('static const unsigned char LEAF_PNG[] PROGMEM = {' + ($hex -join '') + '};')
[void]$sb.AppendLine(('static const size_t LEAF_PNG_LEN = {0};' -f $img.Length))
[void]$sb.AppendLine()
[void]$sb.AppendLine('#endif')

[System.IO.File]::WriteAllText($Out, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
Write-Host ("Wrote {0} ({1} bytes html, {2} bytes png)" -f $Out, $html.Length, $img.Length)
