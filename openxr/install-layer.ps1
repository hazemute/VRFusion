$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Manifest = (Resolve-Path (Join-Path $Root 'XR_APILAYER_VRFusion_capture.json')).Path
$Dll = Join-Path $Root 'VRFusionOpenXRLayer.dll'
if (-not (Test-Path $Dll)) { throw "VRFusionOpenXRLayer.dll not found next to the manifest: $Dll" }
$Key = 'HKCU:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit'
New-Item -Path $Key -Force | Out-Null
New-ItemProperty -Path $Key -Name $Manifest -PropertyType DWord -Value 0 -Force | Out-Null
Write-Host 'VRFusion OpenXR layer registered for the current user.'
Write-Host 'VRFusion 0.5 normally performs this step automatically.'
