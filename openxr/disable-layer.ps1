$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Manifest = (Resolve-Path (Join-Path $Root 'XR_APILAYER_VRFusion_capture.json')).Path
$Key = 'HKCU:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit'
if (Test-Path $Key) {
    New-ItemProperty -Path $Key -Name $Manifest -PropertyType DWord -Value 1 -Force | Out-Null
}
Write-Host 'VRFusion OpenXR layer disabled in the registry.'
