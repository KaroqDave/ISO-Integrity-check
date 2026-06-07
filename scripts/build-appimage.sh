#!/usr/bin/env bash
#
# Builds the Release executable and packages it as a self-contained AppImage
# in standalone/ISO-Integrity-Check-<version>-x86_64.AppImage.
#
# Usage:
#   ./scripts/build-appimage.sh
#
# Optional environment variables:
#   CMAKE_PREFIX_PATH  Path to Qt 6 installation (required if Qt is not on PATH)
#   CMAKE_BUILD_TYPE   CMake build type (default: Release)
#   BUILD_DIR          CMake build directory (default: build-linux)
#   TOOLS_DIR          Where linuxdeploy tools are cached (default: $HOME/.cache/iso-integrity-check-tools)

set -euo pipefail

readonly LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
readonly LINUXDEPLOY_QT_PLUGIN_URL="https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage"

RepoRoot="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
BuildDir="${BUILD_DIR:-$RepoRoot/build-linux}"
ToolsDir="${TOOLS_DIR:-$HOME/.cache/iso-integrity-check-tools}"
OutputRoot="$RepoRoot/standalone"
AppName="ISO-Integrity-Check"

HostOs="$(uname -s)"
HostArch="$(uname -m)"
if [[ "$HostOs" != "Linux" || "$HostArch" != "x86_64" ]]; then
    echo "error: AppImage packaging must run on Linux x86_64 (detected: $HostOs $HostArch)" >&2
    exit 1
fi

# AppImage bundling needs symlinks; WSL drvfs (/mnt/c) does not support them.
if [[ "$RepoRoot" == /mnt/* ]]; then
    AppDir="${TMPDIR:-/tmp}/iso-integrity-check-AppDir"
    BuildOutputDir="${TMPDIR:-/tmp}/iso-integrity-check-appimage-build"
else
    AppDir="${APP_DIR:-$RepoRoot/AppDir}"
    BuildOutputDir="$OutputRoot/appimage-build"
fi

step() {
    echo ""
    echo "==> $1"
}

get_project_version() {
    grep -oE 'project[[:space:]]*\([^)]*VERSION[[:space:]]+[0-9]+\.[0-9]+\.[0-9]+' "$RepoRoot/CMakeLists.txt" \
        | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' \
        | head -1
}

ensure_linuxdeploy() {
    mkdir -p "$ToolsDir"

    local linuxdeploy="$ToolsDir/linuxdeploy-x86_64.AppImage"
    if [[ ! -x "$linuxdeploy" ]]; then
        echo "Downloading linuxdeploy..." >&2
        curl -fsSL -o "$linuxdeploy" "$LINUXDEPLOY_URL"
        chmod +x "$linuxdeploy"
    fi

    local qt_plugin="$ToolsDir/linuxdeploy-plugin-qt-x86_64.AppImage"
    if [[ ! -x "$qt_plugin" ]]; then
        echo "Downloading linuxdeploy Qt plugin..." >&2
        curl -fsSL -o "$qt_plugin" "$LINUXDEPLOY_QT_PLUGIN_URL"
        chmod +x "$qt_plugin"
    fi

    export LINUXDEPLOY_PLUGIN_QT="$qt_plugin"
    echo "$linuxdeploy"
}

Version="$(get_project_version)"
if [[ -z "$Version" ]]; then
    Version="1.0.0"
fi

echo "ISO Integrity Check AppImage build"
echo "  Version   : $Version"
echo "  Build dir : $BuildDir"
echo "  Build type: $CMAKE_BUILD_TYPE"
echo "  Output    : $OutputRoot/$AppName-$Version-x86_64.AppImage"

step "Configuring CMake"
cmake_args=(-S "$RepoRoot" -B "$BuildDir" -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE")
if [[ -n "${CMAKE_PREFIX_PATH:-}" ]]; then
    cmake_args+=(-DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH")
fi
cmake "${cmake_args[@]}"

step "Building"
cmake --build "$BuildDir" --config "$CMAKE_BUILD_TYPE" --target iso-integrity-check

step "Installing to AppDir"
rm -rf "$AppDir"
cmake --install "$BuildDir" --config "$CMAKE_BUILD_TYPE" --prefix "$AppDir/usr"

# Normalize desktop entry line endings (CRLF breaks Icon= lookup on Linux).
sed -i 's/\r$//' "$AppDir/usr/share/applications/iso-integrity-check.desktop"

ExePath="$AppDir/usr/bin/iso-integrity-check"
DesktopPath="$AppDir/usr/share/applications/iso-integrity-check.desktop"
IconPath="$AppDir/usr/share/icons/hicolor/scalable/apps/iso-integrity-check.svg"
IconPngDir="$AppDir/usr/share/icons/hicolor/256x256/apps"
IconPngPath="$IconPngDir/iso-integrity-check.png"

if [[ ! -x "$ExePath" ]]; then
    echo "error: executable not found at '$ExePath'" >&2
    exit 1
fi

mkdir -p "$IconPngDir"
if command -v rsvg-convert >/dev/null 2>&1; then
    rsvg-convert -w 256 -h 256 "$IconPath" -o "$IconPngPath"
elif command -v convert >/dev/null 2>&1; then
    convert -background none "$IconPath" -resize 256x256 "$IconPngPath"
else
    echo "error: install librsvg2-bin (rsvg-convert) or imagemagick to build the AppImage icon" >&2
    exit 1
fi

LinuxDeploy="$(ensure_linuxdeploy)"

if command -v qmake6 >/dev/null 2>&1; then
    export QMAKE="$(command -v qmake6)"
elif command -v qmake >/dev/null 2>&1; then
    export QMAKE="$(command -v qmake)"
fi

step "Creating AppImage"
mkdir -p "$OutputRoot"
OutputPath="$OutputRoot/$AppName-$Version-x86_64.AppImage"
rm -f "$OutputPath"

# linuxdeploy writes the AppImage to the current working directory.
rm -rf "$BuildOutputDir"
mkdir -p "$BuildOutputDir"
pushd "$BuildOutputDir" > /dev/null

"$LinuxDeploy" --appdir "$AppDir" \
    -e "$ExePath" \
    -d "$DesktopPath" \
    -i "$IconPngPath" \
    --plugin qt \
    --output appimage

GeneratedAppImage="$(find "$BuildOutputDir" -maxdepth 1 -name '*.AppImage' -type f | head -1)"
if [[ -z "$GeneratedAppImage" ]]; then
    echo "error: linuxdeploy did not produce an AppImage" >&2
    popd > /dev/null
    exit 1
fi

mv "$GeneratedAppImage" "$OutputPath"
popd > /dev/null
rm -rf "$BuildOutputDir"

step "Done"
echo "AppImage: $OutputPath"
