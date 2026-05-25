# ISO Integrity Check

A simple Windows-friendly tool for checking ISO file integrity with trusted checksums.

![ISO Integrity Check screenshot](docs/screenshot.png)

## Run The GUI

```powershell
python main.py
```

No external Python packages are required. The app uses Python's built-in Tkinter GUI toolkit and standard hashing library.

## Run Without Python

A separate command-line version is available in the `CLI` folder for users who do not have Python installed. It runs through Windows PowerShell and uses the built-in `Get-FileHash` command.

```powershell
.\CLI\iso-integrity-check.cmd -File "C:\Downloads\example.iso" -ChecksumFile "C:\Downloads\SHA256SUMS"
```

See `CLI\README.md` for terminal examples, supported arguments, and exit codes.

## Supported Hashes

- SHA256
- SHA512
- SHA1
- MD5

SHA1 and MD5 are included for older ISO sources, but they are not considered strong for modern security verification. Prefer SHA256 or SHA512 when the vendor provides them.

## Supported Checksum Files

The GUI checksum import button and the separate CLI support common checksum files such as:

- `.sha256`
- `.sha512`
- `.sha1`
- `.md5`
- `.txt`
- `*SUMS`

The app can parse plain checksum files containing only a hash, plus GNU-style files such as:

```text
f2ca1bb6c7e907d06dafe4687e579fce  example.iso
f2ca1bb6c7e907d06dafe4687e579fce *example.iso
```

If the checksum file contains multiple entries, the app prefers the line matching the selected ISO filename. If no filename matches, it uses the first supported checksum it finds.

Checksum import is intended for small text checksum files. Files larger than 1 MB are rejected to keep the desktop UI responsive.

## How To Use

1. Click **Browse...** and select an `.iso` file.
2. Choose the hash type provided by the official download source, or click **Import checksum file...**.
3. Paste the expected checksum if you are not importing it from a checksum file.
4. Click **Calculate / Verify**.

The tool streams the file in chunks, so large ISO files are not loaded fully into memory. If no expected checksum is pasted, the app will still calculate and show the selected hash.

You can right-click the text fields to cut, copy, paste, or select text.

Only trust checksums published by the official operating system or vendor download page.

## Tests

Run the test suite with:

```powershell
python -m unittest
```
