$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Manifest = (Resolve-Path (Join-Path $Root 'XR_APILAYER_VRFusion_capture.json')).Path
$Key = 'HKCU:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit'
New-Item -Path $Key -Force | Out-Null
New-ItemProperty -Path $Key -Name $Manifest -PropertyType DWord -Value 0 -Force | Out-Null
[Environment]::SetEnvironmentVariable('VRFUSION_OPENXR_DISABLE', $null, 'User')
Write-Host 'VRFusion OpenXR layer enabled. The layer remains dormant until VRFusion.exe is running.'
