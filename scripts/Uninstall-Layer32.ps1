$JsonPath = Join-Path "$PSScriptRoot" "XR_APILAYER_MLEDOUR_xr_telemetry-32.json"
Start-Process -FilePath powershell.exe -Verb RunAs -Wait -ArgumentList @"
	& {
		Remove-ItemProperty -Path HKLM:\Software\WOW6432Node\Khronos\OpenXR\1\ApiLayers\Implicit -Name '$jsonPath' -Force | Out-Null
	}
"@
