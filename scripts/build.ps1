$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
$Build = Join-Path $Root 'build'
$Dist = Join-Path $Root 'dist'

function Assert-LastExit([string]$Step) {
    if ($LASTEXITCODE -ne 0) { throw "$Step failed with exit code $LASTEXITCODE" }
}

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw 'CMake was not found in PATH. Install CMake 3.24+ and reopen the terminal.'
}

Write-Host '[VRFusion 0.5] Configuring x64 Release build...'
cmake -S $Root -B $Build -A x64
Assert-LastExit 'CMake configure'

Write-Host '[VRFusion 0.5] Building...'
cmake --build $Build --config Release --parallel
Assert-LastExit 'CMake build'

$Release = Join-Path $Build 'Release'
$Required = @(
    (Join-Path $Release 'VRFusion.exe'),
    (Join-Path $Release 'VRFusionSteamVR.exe'),
    (Join-Path $Release 'VRFusionLauncher.exe'),
    (Join-Path $Release 'VRFusionDoctor.exe'),
    (Join-Path $Release 'VRFusionSharedProbe.exe'),
    (Join-Path $Release 'VRFusionXRView.exe'),
    (Join-Path $Release 'VRFusionOpenXRProbe.exe'),
    (Join-Path $Release 'VRFusionOpenXRLayer.dll'),
    (Join-Path $Release 'openvr_api.dll')
)
foreach ($File in $Required) {
    if (-not (Test-Path $File)) { throw "Expected build output was not produced: $File" }
}

if (Test-Path $Dist) { Remove-Item $Dist -Recurse -Force }
New-Item -ItemType Directory -Path $Dist | Out-Null
New-Item -ItemType Directory -Path (Join-Path $Dist 'openxr-tools') | Out-Null

$Binaries = @(
    'VRFusion.exe','VRFusionSteamVR.exe','VRFusionLauncher.exe','VRFusionDoctor.exe',
    'VRFusionSharedProbe.exe','VRFusionXRView.exe','VRFusionOpenXRProbe.exe',
    'VRFusionOpenXRLayer.dll','openvr_api.dll'
)
foreach ($Name in $Binaries) { Copy-Item (Join-Path $Release $Name) $Dist }
Copy-Item (Join-Path $Root 'openxr\XR_APILAYER_VRFusion_capture.json') $Dist
Copy-Item (Join-Path $Root 'openxr\*.ps1') (Join-Path $Dist 'openxr-tools')
Copy-Item (Join-Path $Root 'vrfusion.ini') $Dist
Copy-Item (Join-Path $Root 'README.md') $Dist
Copy-Item (Join-Path $Root 'STATUS.md') $Dist

Write-Host ''
Write-Host '[VRFusion 0.5] Build completed successfully.'
Write-Host "Output: $Dist"
Write-Host 'Normal use: double-click VRFusion.exe. No OpenXR setup script is required.'
Write-Host 'VRFusion.exe automatically registers the per-user OpenXR layer and selects the best backend.'
