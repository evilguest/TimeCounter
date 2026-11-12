param([ValidateSet('win-x64','win-x86','win-arm64')][string]$Runtime = 'win-x64')
$ErrorActionPreference = 'Stop'
dotnet publish "$PSScriptRoot/TimeCounter/TimeCounter.csproj" -c Release -r $Runtime -o "$PSScriptRoot/dist/$Runtime"
if ($LASTEXITCODE -ne 0) { throw 'Publish failed' }
Get-FileHash "$PSScriptRoot/dist/$Runtime/TimeCounter.exe" -Algorithm SHA256
