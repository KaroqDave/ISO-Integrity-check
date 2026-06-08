<#
.SYNOPSIS
    Generates compile_commands.json so clangd / IntelliSense can resolve Qt and
    MSVC headers in the editor.

.DESCRIPTION
    Visual Studio's CMake generator does not emit a compilation database, so this
    script configures a throwaway Ninja build (build-dev/) inside a Visual Studio
    developer environment and copies the resulting compile_commands.json to the
    repository root, where clangd discovers it automatically. It also runs Qt's
    moc (the *_autogen targets) so generated headers such as test_core.moc exist
    where clangd expects them.

    Run it once after cloning, and again whenever you add, remove, or move source
    files. It does not produce the final executables.

.EXAMPLE
    ./scripts/generate-compile-commands.ps1

.EXAMPLE
    ./scripts/generate-compile-commands.ps1 -QtPath "C:\Qt\6.10.3\msvc2022_64"
#>
[CmdletBinding()]
param(
    [string]$QtPath
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $RepoRoot "build-dev"

function Resolve-QtPath {
    if ($QtPath) { return $QtPath }
    if ($env:CMAKE_PREFIX_PATH) { return $env:CMAKE_PREFIX_PATH }
    $qtRoots = Get-ChildItem "C:\Qt" -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^\d+\.\d+' } |
        Sort-Object Name -Descending
    foreach ($qt in $qtRoots) {
        $kit = Join-Path $qt.FullName "msvc2022_64"
        if (Test-Path (Join-Path $kit "lib\cmake\Qt6\Qt6Config.cmake")) { return $kit }
    }
    return $null
}

function Enter-DeveloperShell {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe not found. Install Visual Studio 2022 (or newer) with the Desktop development with C++ workload."
    }
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsPath) { throw "No Visual Studio C++ toolchain was found." }
    Import-Module (Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
}

$qt = Resolve-QtPath
Write-Host "Repo      : $RepoRoot"
Write-Host "Qt prefix : $(if ($qt) { $qt } else { '(relying on CMAKE_PREFIX_PATH / PATH)' })"

Enter-DeveloperShell

$cmakeArgs = @("-S", $RepoRoot, "-B", $BuildDir, "-G", "Ninja Multi-Config", "-DISO_BUILD_TESTS=ON")
if ($qt) { $cmakeArgs += "-DCMAKE_PREFIX_PATH=$qt" }
$ninja = "C:\Qt\Tools\Ninja\ninja.exe"
if (Test-Path $ninja) { $cmakeArgs += "-DCMAKE_MAKE_PROGRAM=$ninja" }

& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }

$db = Join-Path $BuildDir "compile_commands.json"
if (-not (Test-Path $db)) { throw "compile_commands.json was not generated." }
Copy-Item -LiteralPath $db -Destination (Join-Path $RepoRoot "compile_commands.json") -Force

# Run Qt's moc so generated headers (e.g. test_core.moc) exist for clangd. The
# *_autogen targets only run moc/uic/rcc; they do not compile the executables.
$autogenTargets = @("iso-integrity-check_autogen", "iso-integrity-check-cli_autogen", "iso-core-tests_autogen")
& cmake --build $BuildDir --config Debug --target $autogenTargets
if ($LASTEXITCODE -ne 0) { throw "Running Qt moc (autogen) failed." }

Write-Host ""
Write-Host "compile_commands.json written to the repo root." -ForegroundColor Green
Write-Host "Reload the editor window (or restart clangd) to clear the errors." -ForegroundColor Green
