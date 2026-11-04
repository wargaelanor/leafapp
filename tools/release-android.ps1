# Builds the debug APK and publishes it to a GitHub release so the app can
# update itself from Settings -> "Проверить обновление".
#
# Usage:
#   powershell -File tools\release-android.ps1
#   powershell -File tools\release-android.ps1 -Version 1.3
#   powershell -File tools\release-android.ps1 -SkipBuild
param(
    [string]$Version = "",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    $gradleFile = Join-Path $root "android\app\build.gradle.kts"
    if (-not $Version) {
        $m = Select-String -Path $gradleFile -Pattern 'versionName\s*=\s*"([^"]+)"' | Select-Object -First 1
        if (-not $m) { throw "versionName not found in $gradleFile" }
        $Version = $m.Matches[0].Groups[1].Value
    }
    $tag = "android-v$Version"
    $apk = Join-Path $root "android\app\build\outputs\apk\debug\app-debug.apk"
    $dist = Join-Path $root "android\dist"
    $named = Join-Path $dist "LeafControl-$Version-debug.apk"

    if (-not $SkipBuild) {
        $env:ANDROID_HOME = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { "C:\Android\Sdk" }
        $env:ANDROID_SDK_ROOT = $env:ANDROID_HOME
        & "C:\Gradle\gradle-8.9\bin\gradle.bat" --no-daemon -p (Join-Path $root "android") assembleDebug
        if ($LASTEXITCODE -ne 0) { throw "Gradle build failed" }
    }
    if (-not (Test-Path $apk)) { throw "APK not found: $apk" }

    New-Item -ItemType Directory -Force -Path $dist | Out-Null
    Copy-Item $apk $named -Force
    Write-Host "APK: $named"

    gh auth status *> $null
    if ($LASTEXITCODE -ne 0) { throw "gh is not authenticated" }

    $notes = @"
Автообновление Android-приложения Leaf Control.

- Версия: $Version
- APK подписан тем же debug-ключом, что и предыдущие сборки с этого компьютера.
- Установка: приложение само предложит обновление (Ещё -> Настройки Android -> Проверить обновление).
"@

    $exists = $false
    try {
        gh release view $tag *> $null
        $exists = $LASTEXITCODE -eq 0
    } catch { $exists = $false }

    if ($exists) {
        gh release upload $tag $named --clobber
    } else {
        gh release create $tag $named --title "Android $Version" --notes $notes
    }
    if ($LASTEXITCODE -ne 0) { throw "gh release failed" }
    Write-Host "Release: $tag"
} finally {
    Pop-Location
}
