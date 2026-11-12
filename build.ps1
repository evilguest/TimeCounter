param([switch]$Test)
$ErrorActionPreference = 'Stop'
& "$PSScriptRoot/build-native.ps1" -Test:$Test
