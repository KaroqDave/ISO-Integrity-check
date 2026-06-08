<#
.SYNOPSIS
    Builds the Release executables and exports a self-contained, ready-to-run
    bundle into standalone/ISO-Integrity-Check, then zips it for distribution.

.DESCRIPTION
    Pipeline:
      1. Configure + build the Release executables (windeployqt runs post-build).
      2. Wipe standalone/ISO-Integrity-Check and repopulate it with the exe plus
         the full Qt runtime via `cmake --install`.
      3. Write a fresh README.txt next to the executable.
      4. Zip the folder to standalone/ISO-Integrity-Check-<version>.zip for
         uploading to a GitHub release.

    The standalone/ folder is gitignored; it is regenerated from scratch on each
    run and is meant to be published via Releases rather than committed.

.EXAMPLE
    ./scripts/build-standalone.ps1
#>

[CmdletBinding()]
param(
    [string]$Config = "Release",
    [string]$BuildDir
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) {
    $BuildDir = Join-Path $RepoRoot "build"
}
$AppName    = "ISO-Integrity-Check"
$ExeName    = "iso-integrity-check.exe"
$OutputRoot = Join-Path $RepoRoot "standalone"
$AppDir     = Join-Path $OutputRoot $AppName

function Write-Step([string]$Message) {
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Get-ProjectVersion {
    $cmake = Join-Path $RepoRoot "CMakeLists.txt"
    $content = Get-Content -Raw -LiteralPath $cmake
    if ($content -match 'project\s*\([^)]*VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
        return $Matches[1]
    }
    return "1.0.0"
}

$version = Get-ProjectVersion
Write-Host "ISO Integrity Check standalone build" -ForegroundColor Green
Write-Host "  Version    : $version"
Write-Host "  Config     : $Config"
Write-Host "  Build dir  : $BuildDir"
Write-Host "  Output     : $AppDir"

Write-Step "Configuring CMake"
& cmake -B $BuildDir -S $RepoRoot
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }

Write-Step "Building ($Config)"
& cmake --build $BuildDir --config $Config --target iso-integrity-check iso-integrity-check-cli
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

Write-Step "Exporting standalone bundle"
if (Test-Path -LiteralPath $AppDir) {
    Remove-Item -LiteralPath $AppDir -Recurse -Force
}
New-Item -ItemType Directory -Path $AppDir -Force | Out-Null
& cmake --install $BuildDir --config $Config --prefix $AppDir
if ($LASTEXITCODE -ne 0) { throw "Install/export failed." }

# Drop debug artifacts that the build may leave behind.
Get-ChildItem -Path $AppDir -Recurse -Include "*.pdb", "*.ilk", "*.exp", "*.lib" -ErrorAction SilentlyContinue |
    Remove-Item -Force -ErrorAction SilentlyContinue

$exe = Join-Path $AppDir $ExeName
if (-not (Test-Path -LiteralPath $exe)) {
    throw "Exported executable not found at '$exe'. Export likely failed."
}

Write-Step "Writing README.txt"
$readme = @"
ISO Integrity Check $version - standalone build

Run $ExeName from this folder. Keep the DLLs and plugin folders (platforms,
styles, imageformats, tls, ...) beside the executable when moving or sharing it.

If Windows reports missing Microsoft runtime DLLs on another PC, install the
latest Microsoft Visual C++ Redistributable (x64) from Microsoft.

Project: https://github.com/KaroqDave/ISO-Integrity-check
"@
Set-Content -LiteralPath (Join-Path $AppDir "README.txt") -Value $readme -Encoding UTF8

Write-Step "Creating release archive"
$zipPath = Join-Path $OutputRoot "$AppName-$version.zip"
if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}
Compress-Archive -Path $AppDir -DestinationPath $zipPath -CompressionLevel Optimal

Write-Step "Done"
Write-Host "Bundle : $AppDir" -ForegroundColor Green
Write-Host "Archive: $zipPath" -ForegroundColor Green
