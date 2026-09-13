$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
$ChecksumFile = Join-Path $Root 'PROJECT_CHECKSUMS.sha256'
if (-not (Test-Path $ChecksumFile)) { throw "Missing checksum file: $ChecksumFile" }

$Failures = 0
$Checked = 0
foreach ($Line in Get-Content $ChecksumFile) {
    $Trimmed = $Line.Trim()
    if (-not $Trimmed) { continue }
    if ($Trimmed -notmatch '^([0-9a-fA-F]{64})\s+\*(.+)$') {
        Write-Host "Malformed checksum line: $Line" -ForegroundColor Red
        $Failures++
        continue
    }
    $Expected = $Matches[1].ToLowerInvariant()
    $Relative = $Matches[2].Replace('/', [IO.Path]::DirectorySeparatorChar)
    $Path = Join-Path $Root $Relative
    if (-not (Test-Path $Path -PathType Leaf)) {
        Write-Host "MISSING  $Relative" -ForegroundColor Red
        $Failures++
        continue
    }
    $Actual = (Get-FileHash -Algorithm SHA256 -Path $Path).Hash.ToLowerInvariant()
    $Checked++
    if ($Actual -ne $Expected) {
        Write-Host "FAILED   $Relative" -ForegroundColor Red
        $Failures++
    } else {
        Write-Host "OK       $Relative"
    }
}

if ($Failures -ne 0) { throw "Checksum verification failed: $Failures problem(s), $Checked files checked." }
Write-Host "VRFusion source verification PASS: $Checked files checked."
