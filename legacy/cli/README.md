# Legacy ISO Integrity Check CLI

A zero-Python command-line version of ISO Integrity Check for Windows Terminal, Command Prompt, and PowerShell.

This CLI is preserved as a legacy implementation. The C++/Qt app at the repository root is now the primary version.

The CLI uses Windows PowerShell's built-in `Get-FileHash`, so users do not need Python or external packages installed.

## Run From Windows Terminal

Open Windows Terminal in this folder and use the `.cmd` launcher:

```powershell
.\iso-integrity-check.cmd -File "C:\Downloads\example.iso" -ChecksumFile "C:\Downloads\SHA256SUMS"
```

From the repository root, run:

```powershell
.\legacy\cli\iso-integrity-check.cmd -File "C:\Downloads\example.iso" -ChecksumFile "C:\Downloads\SHA256SUMS"
```

You can also run the PowerShell script directly:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\iso-integrity-check.ps1 -File "C:\Downloads\example.iso" -Algorithm SHA256
```

## Examples

Verify an ISO with a checksum file:

```powershell
.\iso-integrity-check.cmd -File "C:\Downloads\example.iso" -ChecksumFile "C:\Downloads\SHA256SUMS"
```

Verify an ISO with a pasted checksum:

```powershell
.\iso-integrity-check.cmd -File "C:\Downloads\example.iso" -Algorithm SHA256 -Expected "f2ca1bb6c7e907d06dafe4687e579fce7e2d2b7a6f0b9d7f9f8e7c6d5a4b3c20"
```

Calculate a checksum only:

```powershell
.\iso-integrity-check.cmd -File "C:\Downloads\example.iso" -Algorithm SHA512
```

If you do not provide `-Algorithm` when calculating only, the CLI uses `SHA256`.

## Supported Arguments

- `-File <path>`: required ISO or file path to hash.
- `-Expected <checksum>`: optional pasted checksum to verify.
- `-ChecksumFile <path>`: optional checksum text file to import.
- `-Algorithm SHA256|SHA512|SHA1|MD5`: optional hash algorithm.

`-Expected` and `-ChecksumFile` cannot be used together. When `-Expected` is used without `-Algorithm`, the CLI infers the algorithm from the checksum length.

## Supported Checksum Files

The CLI can parse plain checksum files, GNU-style checksum lines, and BSD-style checksum lines:

```text
f2ca1bb6c7e907d06dafe4687e579fce7e2d2b7a6f0b9d7f9f8e7c6d5a4b3c20  example.iso
f2ca1bb6c7e907d06dafe4687e579fce7e2d2b7a6f0b9d7f9f8e7c6d5a4b3c20 *example.iso
SHA256 (example.iso) = f2ca1bb6c7e907d06dafe4687e579fce7e2d2b7a6f0b9d7f9f8e7c6d5a4b3c20
```

If the file contains multiple checksums, the CLI prefers the entry that exactly matches the ISO filename. If no filename matches, it uses the first supported checksum it finds.

## Exit Codes

- `0`: checksum matched, or hash-only calculation succeeded.
- `1`: checksum mismatch.
- `2`: invalid input, missing file, unsupported checksum file, or runtime error.
