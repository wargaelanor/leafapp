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

    # Keep release notes in a UTF-8 .md file. Inline Russian in a .ps1 is read
    # as ANSI by Windows PowerShell and turns into mojibake on GitHub.
    $notesFile = Join-Path $root "android\release-notes.md"
    if (-not (Test-Path $notesFile)) {
        $notes = "Android $Version. See the repository for details."
    } else {
        $notes = [System.IO.File]::ReadAllText($notesFile, [System.Text.Encoding]::UTF8)
        $notes = $notes.Replace("{{VERSION}}", $Version)
    }
    # gh on Windows can read --notes as ANSI. Always pass a real UTF-8 file.
    $notesPath = Join-Path $env:TEMP "leaf-android-$Version-notes.md"
    [System.IO.File]::WriteAllText(
        $notesPath, $notes, (New-Object System.Text.UTF8Encoding($false))
    )

    $exists = $false
    try {
        gh release view $tag *> $null
        $exists = $LASTEXITCODE -eq 0
    } catch { $exists = $false }

    if ($exists) {
        gh release upload $tag $named --clobber
        if ($LASTEXITCODE -ne 0) { throw "gh release upload failed" }
        $repo = gh repo view --json nameWithOwner -q .nameWithOwner
        $releaseId = gh api "repos/$repo/releases/tags/$tag" --jq ".id"
        $payload = @{ body = $notes } | ConvertTo-Json -Depth 3
        $jsonPath = Join-Path $env:TEMP "leaf-android-$Version-patch.json"
        [System.IO.File]::WriteAllText(
            $jsonPath, $payload, (New-Object System.Text.UTF8Encoding($false))
        )
        # gh api --input re-encodes the body on Windows. curl sends the UTF-8
        # JSON file byte-for-byte, so Russian notes stay readable.
        $token = gh auth token
        curl.exe -sS --max-time 30 -X PATCH `
            "https://api.github.com/repos/$repo/releases/$releaseId" `
            -H "Authorization: Bearer $token" `
            -H "Accept: application/vnd.github+json" `
            -H "Content-Type: application/json; charset=utf-8" `
            --data-binary "@$jsonPath" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "release notes update failed" }
    } else {
        gh release create $tag $named --title "Android $Version" --notes-file $notesPath
    }
    if ($LASTEXITCODE -ne 0) { throw "gh release failed" }
    Write-Host "Release: $tag"
} finally {
    Pop-Location
}
