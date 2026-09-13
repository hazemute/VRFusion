$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
$Build = Join-Path $Root 'build'
$Dist = Join-Path $Root 'dist'

function Assert-LastExit([string]$Step) {
    if ($LASTEXITCODE -ne 0) {
        throw "$Step failed with exit code $LASTEXITCODE"
    }
}

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw 'CMake was not found in PATH. Install CMake 3.24+ and reopen the terminal.'
}

Write-Host '[VRFusion 0.2] Configuring x64 Release build...'
cmake -S $Root -B $Build -A x64
Assert-LastExit 'CMake configure'

Write-Host '[VRFusion 0.2] Building...'
cmake --build $Build --config Release --parallel
Assert-LastExit 'CMake build'

$Release = Join-Path $Build 'Release'
$Required = @(
    (Join-Path $Release 'VRFusion.exe'),
    (Join-Path $Release 'VRFusionSharedProbe.exe'),
    (Join-Path $Release 'openvr_api.dll')
)
foreach ($File in $Required) {
    if (-not (Test-Path $File)) {
        throw "Expected build output was not produced: $File"
    }
}

if (Test-Path $Dist) { Remove-Item $Dist -Recurse -Force }
New-Item -ItemType Directory -Path $Dist | Out-Null

Copy-Item (Join-Path $Release 'VRFusion.exe') $Dist
Copy-Item (Join-Path $Release 'VRFusionSharedProbe.exe') $Dist
Copy-Item (Join-Path $Release 'openvr_api.dll') $Dist
Copy-Item (Join-Path $Root 'vrfusion.ini') $Dist
Copy-Item (Join-Path $Root 'README.md') $Dist
Copy-Item (Join-Path $Root 'STATUS.md') $Dist

Write-Host ''
Write-Host '[VRFusion 0.2] Build completed successfully.'
Write-Host "Output: $Dist"
Write-Host 'Run VRFusion.exe with SteamVR active.'
Write-Host 'Optional: run VRFusionSharedProbe.exe while VRFusion is running to verify GPU sharing.'
