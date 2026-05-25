[CmdletBinding()]
param(
    [string]$File,
    [string]$Expected,
    [string]$ChecksumFile,
    [string]$Algorithm
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$SupportedHashes = @{
    "SHA256" = @{ HexLength = 64 }
    "SHA512" = @{ HexLength = 128 }
    "SHA1" = @{ HexLength = 40 }
    "MD5" = @{ HexLength = 32 }
}

$HashesByLength = @{
    64 = "SHA256"
    128 = "SHA512"
    40 = "SHA1"
    32 = "MD5"
}

$ChecksumTokenPattern = "(?<![0-9a-fA-F])([0-9a-fA-F]{128}|[0-9a-fA-F]{64}|[0-9a-fA-F]{40}|[0-9a-fA-F]{32})(?![0-9a-fA-F])"
$MaxChecksumFileSize = 1024 * 1024

function Normalize-Checksum {
    param([string]$Value)

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return ""
    }

    return $Value.Trim().ToLowerInvariant()
}

function Normalize-Algorithm {
    param([string]$Value)

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return ""
    }

    $normalized = $Value.Trim().ToUpperInvariant()
    if (-not $SupportedHashes.ContainsKey($normalized)) {
        throw "Unsupported hash algorithm: $Value. Use SHA256, SHA512, SHA1, or MD5."
    }

    return $normalized
}

function Get-AlgorithmFromChecksum {
    param([string]$Checksum)

    if (-not $HashesByLength.ContainsKey($Checksum.Length)) {
        throw "Unable to infer hash algorithm. Provide -Algorithm or use a 32, 40, 64, or 128 character checksum."
    }

    return $HashesByLength[$Checksum.Length]
}

function Assert-ValidExpectedChecksum {
    param(
        [string]$Checksum,
        [string]$HashAlgorithm
    )

    if (-not $SupportedHashes.ContainsKey($HashAlgorithm)) {
        throw "Unsupported hash algorithm: $HashAlgorithm"
    }

    $expectedLength = $SupportedHashes[$HashAlgorithm].HexLength
    if ($Checksum.Length -ne $expectedLength) {
        throw "$HashAlgorithm checksums must be $expectedLength hexadecimal characters. The provided value has $($Checksum.Length)."
    }

    if ($Checksum -notmatch "^[0-9a-f]+$") {
        throw "$HashAlgorithm checksums can only contain hexadecimal characters."
    }
}

function Resolve-InputFile {
    param(
        [string]$Path,
        [string]$Description
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "Choose an ISO file with -File."
    }

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "The selected $Description does not exist or is not a file: $Path"
    }

    return (Resolve-Path -LiteralPath $Path).Path
}

function Get-ChecksumFilename {
    param(
        [string]$Line,
        [int]$TokenStart,
        [int]$TokenEnd
    )

    $before = $Line.Substring(0, $TokenStart).Trim()
    $after = $Line.Substring($TokenEnd).Trim()

    $bsdMatch = [regex]::Match($before, "^[A-Za-z0-9-]+\s*\((.+)\)\s*=$")
    if ($bsdMatch.Success) {
        return $bsdMatch.Groups[1].Value.Trim().Trim([char]'"')
    }

    $filename = $after
    if ([string]::IsNullOrWhiteSpace($filename)) {
        $filename = $before
    }

    if ([string]::IsNullOrWhiteSpace($filename)) {
        return ""
    }

    $filename = $filename.TrimStart([char]"*").Trim()
    if ($filename.StartsWith("(") -and $filename.Contains(")")) {
        $filename = $filename.Substring(1, $filename.IndexOf(")") - 1)
    }
    if ($filename.StartsWith("=")) {
        $filename = $filename.Substring(1).Trim()
    }

    return $filename.Trim().Trim([char]'"')
}

function Parse-ChecksumLine {
    param(
        [string]$Line,
        [int]$LineNumber
    )

    $stripped = $Line.Trim()
    if ([string]::IsNullOrWhiteSpace($stripped) -or $stripped.StartsWith("#") -or $stripped.StartsWith(";")) {
        return $null
    }

    $match = [regex]::Match($stripped, $ChecksumTokenPattern)
    if (-not $match.Success) {
        return $null
    }

    $checksum = $match.Groups[1].Value.ToLowerInvariant()
    $hashAlgorithm = $HashesByLength[$checksum.Length]
    $filename = Get-ChecksumFilename -Line $stripped -TokenStart $match.Index -TokenEnd ($match.Index + $match.Length)

    return [pscustomobject]@{
        Algorithm = $hashAlgorithm
        Checksum = $checksum
        LineNumber = $LineNumber
        Filename = $filename
        RawLine = $stripped
    }
}

function Test-ChecksumCandidateMatchesIso {
    param(
        [pscustomobject]$Candidate,
        [string]$IsoName
    )

    if ([string]::IsNullOrWhiteSpace($Candidate.Filename)) {
        return $false
    }

    $normalizedPath = $Candidate.Filename -replace "\\", "/"
    $parts = $normalizedPath.Split("/")
    $filename = $parts[$parts.Length - 1].ToLowerInvariant()
    return $filename -eq $IsoName
}

function Read-ChecksumFile {
    param(
        [string]$ChecksumPath,
        [string]$IsoPath
    )

    $resolvedChecksumPath = Resolve-InputFile -Path $ChecksumPath -Description "checksum file"
    $checksumItem = Get-Item -LiteralPath $resolvedChecksumPath
    if ($checksumItem.Length -gt $MaxChecksumFileSize) {
        throw "The selected checksum file is too large. Choose a checksum text file under 1024 KB."
    }

    $isoName = [System.IO.Path]::GetFileName($IsoPath).ToLowerInvariant()
    $firstCandidate = $null
    $lineNumber = 0

    foreach ($line in [System.IO.File]::ReadLines($resolvedChecksumPath)) {
        $lineNumber += 1
        $candidate = Parse-ChecksumLine -Line $line -LineNumber $lineNumber
        if ($null -eq $candidate) {
            continue
        }

        if ($null -eq $firstCandidate) {
            $firstCandidate = $candidate
        }

        if (Test-ChecksumCandidateMatchesIso -Candidate $candidate -IsoName $isoName) {
            return $candidate
        }
    }

    if ($null -ne $firstCandidate) {
        return $firstCandidate
    }

    throw "No supported checksum was found in the selected file."
}

function Write-VerificationOutput {
    param(
        [string]$ResolvedFile,
        [string]$HashAlgorithm,
        [string]$ComputedChecksum,
        [string]$ExpectedChecksum,
        [string]$ResultText,
        [string]$ChecksumSource
    )

    Write-Output "File: $ResolvedFile"
    if (-not [string]::IsNullOrWhiteSpace($ChecksumSource)) {
        Write-Output "Checksum source: $ChecksumSource"
    }
    Write-Output "Algorithm: $HashAlgorithm"
    if (-not [string]::IsNullOrWhiteSpace($ExpectedChecksum)) {
        Write-Output "Expected: $ExpectedChecksum"
    }
    Write-Output "Computed: $ComputedChecksum"
    Write-Output "Result: $ResultText"
}

try {
    $hasExpected = -not [string]::IsNullOrWhiteSpace($Expected)
    $hasChecksumFile = -not [string]::IsNullOrWhiteSpace($ChecksumFile)

    if ($hasExpected -and $hasChecksumFile) {
        throw "Use either -Expected or -ChecksumFile, not both."
    }

    $normalizedAlgorithm = Normalize-Algorithm -Value $Algorithm
    $resolvedFile = Resolve-InputFile -Path $File -Description "file"

    $expectedChecksum = ""
    $checksumSource = ""
    $hashAlgorithm = $normalizedAlgorithm

    if ($hasChecksumFile) {
        $parsedChecksum = Read-ChecksumFile -ChecksumPath $ChecksumFile -IsoPath $resolvedFile
        $expectedChecksum = $parsedChecksum.Checksum
        $hashAlgorithm = $parsedChecksum.Algorithm
        if (-not [string]::IsNullOrWhiteSpace($normalizedAlgorithm) -and $normalizedAlgorithm -ne $hashAlgorithm) {
            throw "Checksum file contains $hashAlgorithm, but -Algorithm was $normalizedAlgorithm."
        }

        $checksumSource = "$ChecksumFile, line $($parsedChecksum.LineNumber)"
        if (-not [string]::IsNullOrWhiteSpace($parsedChecksum.Filename)) {
            $checksumSource = "$checksumSource ($($parsedChecksum.Filename))"
        }
    } elseif ($hasExpected) {
        $expectedChecksum = Normalize-Checksum -Value $Expected
        if ([string]::IsNullOrWhiteSpace($hashAlgorithm)) {
            $hashAlgorithm = Get-AlgorithmFromChecksum -Checksum $expectedChecksum
        }
        Assert-ValidExpectedChecksum -Checksum $expectedChecksum -HashAlgorithm $hashAlgorithm
    } else {
        if ([string]::IsNullOrWhiteSpace($hashAlgorithm)) {
            $hashAlgorithm = "SHA256"
        }
    }

    $computedChecksum = (Get-FileHash -LiteralPath $resolvedFile -Algorithm $hashAlgorithm).Hash.ToLowerInvariant()

    if ([string]::IsNullOrWhiteSpace($expectedChecksum)) {
        Write-VerificationOutput -ResolvedFile $resolvedFile -HashAlgorithm $hashAlgorithm -ComputedChecksum $computedChecksum -ExpectedChecksum "" -ResultText "Checksum calculated. Provide -Expected or -ChecksumFile to verify." -ChecksumSource ""
        exit 0
    }

    if ($computedChecksum -eq $expectedChecksum) {
        Write-VerificationOutput -ResolvedFile $resolvedFile -HashAlgorithm $hashAlgorithm -ComputedChecksum $computedChecksum -ExpectedChecksum $expectedChecksum -ResultText "MATCH - The ISO checksum matches the expected value." -ChecksumSource $checksumSource
        exit 0
    }

    Write-VerificationOutput -ResolvedFile $resolvedFile -HashAlgorithm $hashAlgorithm -ComputedChecksum $computedChecksum -ExpectedChecksum $expectedChecksum -ResultText "MISMATCH - The ISO checksum does not match the expected value." -ChecksumSource $checksumSource
    exit 1
} catch {
    [Console]::Error.WriteLine("Error: {0}" -f $_.Exception.Message)
    exit 2
}
