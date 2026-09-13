$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Manifest = (Resolve-Path (Join-Path $Root 'XR_APILAYER_VRFusion_capture.json')).Path
$Key = 'HKCU:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit'
if (Test-Path $Key) { Remove-ItemProperty -Path $Key -Name $Manifest -ErrorAction SilentlyContinue }
[Environment]::SetEnvironmentVariable('VRFUSION_OPENXR', $null, 'User')
[Environment]::SetEnvironmentVariable('VRFUSION_OPENXR_DISABLE', $null, 'User')
Write-Host 'VRFusion OpenXR registration removed for the current user.'
