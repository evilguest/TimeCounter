param([switch]$Test)
$ErrorActionPreference = 'Stop'
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Install Visual Studio Build Tools with Desktop development with C++ and a Windows 10 SDK.' }
Import-Module "$installation/Common7/Tools/Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $installation -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
$output = Join-Path $PSScriptRoot 'dist/native'
New-Item -ItemType Directory -Force $output | Out-Null
Push-Location "$PSScriptRoot/native"
try {
    & rc.exe /nologo "/fo$output/app.res" app.rc
    if ($LASTEXITCODE -ne 0) { throw 'Resource compilation failed' }
    $common = @('/nologo','/std:c++17','/utf-8','/W4','/O1','/MT','/EHsc','/Gy','/Gw','/GL','/DNOMINMAX','/DUNICODE','/D_UNICODE','/D_WIN32_WINNT=0x0A00',"/Fo$output/")
    $libraries = @('user32.lib','gdi32.lib','advapi32.lib','wtsapi32.lib','comctl32.lib','comdlg32.lib','shell32.lib','ole32.lib','bcrypt.lib','shcore.lib','ws2_32.lib')
    & cl.exe @common core.cpp network.cpp ui.cpp "/Fe$output/TimeCounter.exe" "$output/app.res" /link /LTCG /OPT:REF /OPT:ICF /MANIFEST:NO /SUBSYSTEM:WINDOWS @libraries
    if ($LASTEXITCODE -ne 0) { throw 'Native compilation failed' }
    if ($Test) {
        & cl.exe @common /DTIMECOUNTER_TEST core.cpp network.cpp ui.cpp tests.cpp "/Fe$output/TimeCounter.Tests.exe" "$output/app.res" /link /LTCG /OPT:REF /OPT:ICF /MANIFEST:NO /SUBSYSTEM:CONSOLE @libraries
        if ($LASTEXITCODE -ne 0) { throw 'Test compilation failed' }
        & "$output/TimeCounter.Tests.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Native tests failed' }
    }
    Get-Item "$output/TimeCounter.exe" | Select-Object Name, Length
    & dumpbin.exe /nologo /dependents "$output/TimeCounter.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Dependency inspection failed' }
    Write-Output ('SHA256: ' + (Get-FileHash "$output/TimeCounter.exe" -Algorithm SHA256).Hash)
} finally { Pop-Location }
