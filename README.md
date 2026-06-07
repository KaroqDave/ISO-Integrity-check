# ISO Integrity Check

A Windows desktop app for checking ISO file integrity with trusted checksums.

The primary implementation is now a C++/Qt app that builds to a native `.exe`. The older Python GUI and PowerShell CLI remain available under `legacy/`, but they are no longer the main development target.

## Download (Ready To Run)

Grab the latest standalone build from the [Releases page](https://github.com/KaroqDave/ISO-Integrity-check/releases):

1. Download `ISO-Integrity-Check-<version>.zip`.
2. Extract it anywhere.
3. Run `iso-integrity-check.exe`.

Keep the DLLs and plugin folders next to the executable. If Windows reports missing runtime DLLs, install the latest [Microsoft Visual C++ Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe).

## Build The C++ App

Prerequisites:

- Visual Studio 2022 with the MSVC C++ toolchain.
- CMake 3.21 or newer.
- Qt 6 with the `Core` and `Widgets` components.

Configure and build:

```powershell
cmake -S . -B build -DCMAKE_PREFIX_PATH="C:\Qt\6.10.3\msvc2022_64"
cmake --build build --config Release
```

The executable is produced at:

```text
build\Release\iso-integrity-check.exe
```

The build runs `windeployqt` after compiling the GUI, so Qt runtime DLLs and plugins are copied beside the executable. Adjust `CMAKE_PREFIX_PATH` if your Qt version or kit path changes.

## Standalone Build (Export For Distribution)

To produce a clean, self-contained folder (and a zip ready for a release), run:

```powershell
./scripts/build-standalone.ps1
```

This builds the Release executable, exports it together with the full Qt runtime to `standalone/ISO-Integrity-Check`, and creates `standalone/ISO-Integrity-Check-<version>.zip`. The `standalone/` folder is regenerated on each run and is not tracked in git.

## Performance

Hashing uses the Windows CNG (BCrypt) API, which is hardware-accelerated (SHA-NI) when the CPU supports it, and overlaps file reading with hashing for large ISO files. On other platforms it falls back to Qt's `QCryptographicHash`.

## Supported Hashes

- SHA256
- SHA512
- SHA1
- MD5

SHA1 and MD5 are included for older ISO sources, but they are not considered strong for modern security verification. Prefer SHA256 or SHA512 when the vendor provides them.

## Supported Checksum Files

The app can import common checksum files such as:

- `.sha256`
- `.sha512`
- `.sha1`
- `.md5`
- `.txt`
- `*SUMS`

It supports plain checksum files, GNU-style files, and BSD-style files:

```text
f2ca1bb6c7e907d06dafe4687e579fce  example.iso
f2ca1bb6c7e907d06dafe4687e579fce *example.iso
SHA256 (example.iso) = f2ca1bb6c7e907d06dafe4687e579fce
```

If a checksum file contains multiple entries, the app prefers the line matching the selected ISO filename. If no filename matches, it uses the first supported checksum it finds. Checksum files larger than 1 MB are rejected.

## How To Use

1. Click **Browse...** and select an `.iso` file.
2. Choose the hash type provided by the official download source, or click **Import checksum file...**.
3. Paste the expected checksum if you are not importing it from a checksum file.
4. Click **Calculate / Verify**.

The app streams files in chunks, so large ISO files are not loaded fully into memory. If no expected checksum is pasted, it will still calculate and show the selected hash.

Only trust checksums published by the official operating system or vendor download page.

## Legacy Versions

- Python GUI: `legacy/python/main.py`
- Python tests: `legacy/python/test_main.py`
- PowerShell CLI: `legacy/cli/iso-integrity-check.cmd`

Run the legacy Python tests with:

```powershell
python -m unittest discover -s legacy\python
```
