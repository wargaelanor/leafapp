# Builds the client demo page: one self-contained HTML with the car pictures
# inlined as data URIs, so it can be sent as a single file or opened offline.
#
#   powershell -File tools\mkdemo.ps1
#
# Output: demo\leafcontrol-demo.html
param(
  [string]$Template = "$PSScriptRoot\..\demo\demo-template.html",
  [string]$Out = "$PSScriptRoot\..\demo\leafcontrol-demo.html"
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function As-DataUri([string]$path, [string]$mime) {
  $bytes = [System.IO.File]::ReadAllBytes($path)
  return ('data:{0};base64,{1}' -f $mime, [Convert]::ToBase64String($bytes))
}

$images = [ordered]@{
  '@@LEAF@@'   = 'leaf.png'
  '@@LIGHTS@@' = 'car-lights.png'
  '@@CABLE@@'  = 'car-cable.png'
  '@@W1@@'     = 'car-wheel1.png'
  '@@W2@@'     = 'car-wheel2.png'
  '@@W3@@'     = 'car-wheel3.png'
  '@@W4@@'     = 'car-wheel4.png'
}

$html = [System.IO.File]::ReadAllText((Resolve-Path $Template), [System.Text.Encoding]::UTF8)
$total = 0

foreach ($token in $images.Keys) {
  $name = $images[$token]
  $file = Join-Path $root $name
  if (-not (Test-Path $file)) {
    $file = Join-Path (Split-Path -Parent $Template) $name
  }
  if (-not (Test-Path $file)) {
    throw "missing image: $name"
  }
  $uri = As-DataUri $file 'image/png'
  $html = $html.Replace($token, $uri)
  $total += $uri.Length
  Write-Host ("  {0}: {1} KB inline" -f $name, [int]((Get-Item $file).Length / 1024))
}

if ($html.Contains('@@')) {
  throw 'template still has unreplaced placeholders'
}

[System.IO.File]::WriteAllText([System.IO.Path]::GetFullPath($Out), $html, (New-Object System.Text.UTF8Encoding($false)))
Write-Host ("Wrote {0} ({1} KB)" -f $Out, [int]((Get-Item $Out).Length / 1024))
