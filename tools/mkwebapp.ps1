# Generates webapp_html.h from the PWA (index.html + pictures + fonts) for ESP32 PROGMEM.
# Usage: powershell -File tools/mkwebapp.ps1 [-Out <path\to\webapp_html.h>]
param([string]$Out, [ValidateSet('t2can','tcall')][string]$Board = 't2can')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

# PWA location: repo root (mirror layout) or <root>/leafapp (dev tree layout)
$app = if (Test-Path (Join-Path $root 'index.html')) { $root } else { Join-Path $root 'leafapp' }
if (-not $Out) { $Out = Join-Path $root 'firmware\LeafControl\webapp_html.h' }

$html = [System.IO.File]::ReadAllText((Join-Path $app 'index.html'), [System.Text.Encoding]::UTF8)

# Demo mode is a showcase for the PWA on the web only: strip it from the copy
# that lives in the board's flash (it costs space and is never used there).
$html = [regex]::Replace($html, '(?s)/\*DEMO-BEGIN\*/.*?/\*DEMO-END\*/', '')
$html = [regex]::Replace($html, '(?s)<!--DEMO-BEGIN-->.*?<!--DEMO-END-->', '')
$html = $html.Replace("let mode = 'local';", "const DEMO = false;`nlet mode = 'local';")
# Drop the (now removed) demo hooks so the board copy has no dead references.
$html = [regex]::Replace($html, '(?m)^[ \t]*if\(DEMO\) return demoCommand\(type, quiet\);\r?\n', '')
$html = [regex]::Replace($html, '(?m)^[ \t]*if\(DEMO\)\{ demoStart\(\); return; \}\r?\n', '')

if ($html -match '\)"rawliteral') { throw 'index.html contains rawliteral terminator - change delimiter' }
# Keep the source readable, but omit indentation and empty lines from the
# embedded copy. On the SIM800 build every saved byte matters near the OTA
# partition limit; this transformation does not alter HTML/JS token spacing.
$html = [regex]::Replace($html, '(?m)^[ \t]+', '')
$html = [regex]::Replace($html, '(?m)^\r?\n', '')

# Vehicle picture: the measured 660x367 PNG.
$imgFile = 'leaf.png'; $imgType = 'image/png'
if (-not (Test-Path (Join-Path $app $imgFile))) { throw "missing $imgFile" }
$img = [System.IO.File]::ReadAllBytes((Join-Path $app $imgFile))
$hex = for ($i = 0; $i -lt $img.Length; $i++) {
  if ($i % 16 -eq 0) { "`n  " } else { ' ' }
  '0x{0:x2},' -f $img[$i]
}

# Gzip copy of the page. The board is often reached through its own access
# point, where throughput is poor; a compressed body is about a quarter of the
# size and loads several times faster.
$htmlBytes = [System.Text.Encoding]::UTF8.GetBytes($html)
$ms = New-Object System.IO.MemoryStream
$gz = New-Object System.IO.Compression.GZipStream($ms, [System.IO.Compression.CompressionLevel]::Optimal, $true)
$gz.Write($htmlBytes, 0, $htmlBytes.Length)
$gz.Dispose()
$gzBytes = $ms.ToArray()
$ms.Dispose()
$gzHex = for ($i = 0; $i -lt $gzBytes.Length; $i++) {
  if ($i % 16 -eq 0) { "`n  " } else { ' ' }
  '0x{0:x2},' -f $gzBytes[$i]
}

# Lightweight page (/lite): a few kilobytes, no images, no service worker. It is
# the entry point when the link is too poor for the full app.
$litePath = Join-Path $app 'lite.html'
$liteHtml = ''
$liteGzBytes = @()
$liteGzHex = @()
if (Test-Path $litePath) {
    $liteHtml = [System.IO.File]::ReadAllText($litePath, [System.Text.Encoding]::UTF8)
    $liteHtml = [regex]::Replace($liteHtml, '(?m)^[ \t]+', '')
    $liteHtml = [regex]::Replace($liteHtml, '(?m)^\r?\n', '')
    if ($liteHtml -match '\)"rawliteral') { throw 'lite.html contains rawliteral terminator' }
    $liteBytes = [System.Text.Encoding]::UTF8.GetBytes($liteHtml)
    $lm = New-Object System.IO.MemoryStream
    $lg = New-Object System.IO.Compression.GZipStream($lm, [System.IO.Compression.CompressionLevel]::Optimal, $true)
    $lg.Write($liteBytes, 0, $liteBytes.Length)
    $lg.Dispose()
    $liteGzBytes = $lm.ToArray()
    $lm.Dispose()
    $liteGzHex = for ($i = 0; $i -lt $liteGzBytes.Length; $i++) {
        if ($i % 16 -eq 0) { "`n  " } else { ' ' }
        '0x{0:x2},' -f $liteGzBytes[$i]
    }
}

# First-run page: choose the operating mode once. Kept as a separate source
# file so regenerating this header cannot silently drop it.
$setupPath = Join-Path $app 'setup.html'
$setupHtml = ''
if (Test-Path $setupPath) {
    $setupHtml = [System.IO.File]::ReadAllText($setupPath, [System.Text.Encoding]::UTF8)
    $setupHtml = [regex]::Replace($setupHtml, '(?m)^[ \t]+', '')
    $setupHtml = [regex]::Replace($setupHtml, '(?m)^\r?\n', '')
    if ($setupHtml -match '\)"setup') { throw 'setup.html contains setup terminator' }
}

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('// GENERATED FILE - do not edit. Run tools/mkwebapp.ps1')
[void]$sb.AppendLine('#ifndef WEBAPP_HTML_H')
[void]$sb.AppendLine('#define WEBAPP_HTML_H')
[void]$sb.AppendLine('#include <Arduino.h>')
[void]$sb.AppendLine()
# T-Call (4 MB) must fit two OTA slots: embed only the gzip copies, no raw HTML.
if ($Board -eq 'tcall') {
    [void]$sb.AppendLine('#define WEBAPP_GZ_ONLY 1')
    [void]$sb.AppendLine('static const char INDEX_HTML[] PROGMEM = "";')
}
if ($Board -ne 'tcall') {
    [void]$sb.AppendLine('static const char INDEX_HTML[] PROGMEM = R"rawliteral(')
    [void]$sb.Append($html)
    [void]$sb.AppendLine(')rawliteral";')
}
[void]$sb.AppendLine('static const unsigned char INDEX_HTML_GZ[] PROGMEM = {' + ($gzHex -join '') + '};')
[void]$sb.AppendLine(('static const size_t INDEX_HTML_GZ_LEN = {0};' -f $gzBytes.Length))
if ($liteHtml) {
    if ($Board -ne 'tcall') {
        [void]$sb.AppendLine('static const char LITE_HTML[] PROGMEM = R"rawliteral(')
        [void]$sb.Append($liteHtml)
        [void]$sb.AppendLine(')rawliteral";')
    } else {
        [void]$sb.AppendLine('static const char LITE_HTML[] PROGMEM = "";')
    }
    [void]$sb.AppendLine('static const unsigned char LITE_HTML_GZ[] PROGMEM = {' + ($liteGzHex -join '') + '};')
    [void]$sb.AppendLine(('static const size_t LITE_HTML_GZ_LEN = {0};' -f $liteGzBytes.Length))
} else {
    [void]$sb.AppendLine('static const char LITE_HTML[] PROGMEM = "";')
    [void]$sb.AppendLine('static const unsigned char LITE_HTML_GZ[] PROGMEM = {0};')
    [void]$sb.AppendLine('static const size_t LITE_HTML_GZ_LEN = 0;')
}
[void]$sb.AppendLine()
[void]$sb.AppendLine('static const unsigned char LEAF_IMG[] PROGMEM = {' + ($hex -join '') + '};')
[void]$sb.AppendLine(('static const size_t LEAF_IMG_LEN = {0};' -f $img.Length))
[void]$sb.AppendLine(('static const char LEAF_IMG_TYPE[] PROGMEM = "{0}";' -f $imgType))
[void]$sb.AppendLine()

# Status-picture overlays and the two web fonts, served from the board itself.
# The T-Call has only 4 MB of flash: drop the overlays and fonts there and let
# the page fall back to system fonts / no layers. Saves ~260 KB.
$assets = [ordered]@{}
if ($Board -ne 'tcall') {
    $assets = [ordered]@{
        'car-lights.png'           = 'image/png'
        'car-cable.png'            = 'image/png'
        'car-wheel1.png'           = 'image/png'
        'car-wheel2.png'           = 'image/png'
        'car-wheel3.png'           = 'image/png'
        'car-wheel4.png'           = 'image/png'
        'fonts/nissan_regular.ttf' = 'font/ttf'
        'fonts/nissan_light.ttf'   = 'font/ttf'
    }
}
$entries = @()
foreach ($name in $assets.Keys) {
    $file = Join-Path $app $name
    if (-not (Test-Path $file)) { continue }
    $bytes = [System.IO.File]::ReadAllBytes($file)
    $var = ($name -replace '[^A-Za-z0-9]', '_').ToUpper()
    $h = for ($i = 0; $i -lt $bytes.Length; $i++) {
        if ($i % 16 -eq 0) { "`n  " } else { ' ' }
        '0x{0:x2},' -f $bytes[$i]
    }
    [void]$sb.AppendLine(('static const unsigned char {0}[] PROGMEM = {{{1}}};' -f $var, ($h -join '')))
    $entries += ('  {{"/{0}", {1}, sizeof({1}), "{2}"}},' -f $name, $var, $assets[$name])
}
[void]$sb.AppendLine('struct WebAppAsset { const char* path; const unsigned char* data; size_t len; const char* type; };')
if ($entries.Count -gt 0) {
    [void]$sb.AppendLine('static const WebAppAsset WEBAPP_ASSETS[] = {')
    foreach ($e in $entries) { [void]$sb.AppendLine($e) }
    [void]$sb.AppendLine('};')
    [void]$sb.AppendLine(('static const size_t WEBAPP_ASSET_COUNT = {0};' -f $entries.Count))
} else {
    [void]$sb.AppendLine('static const WebAppAsset WEBAPP_ASSETS[] = {{"/none", nullptr, 0, "text/plain"}};')
    [void]$sb.AppendLine('static const size_t WEBAPP_ASSET_COUNT = 0;')
}
[void]$sb.AppendLine()
if ($setupHtml) {
    [void]$sb.AppendLine('static const char SETUP_HTML[] PROGMEM = R"setup(')
    [void]$sb.Append($setupHtml)
    [void]$sb.AppendLine(')setup";')
}

[void]$sb.AppendLine('#endif')

[System.IO.File]::WriteAllText($Out, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
Write-Host ("Wrote {0}: html {1} B (gzip {2} B), image {3} B" -f $Out, $htmlBytes.Length, $gzBytes.Length, $img.Length)
