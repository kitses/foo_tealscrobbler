@echo off
setlocal
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo Visual Studio with the C++ workload was not found. & exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
pushd "%~dp0.."
msbuild src\foo_tealscrobbler.vcxproj -m -p:Configuration=Release -p:Platform=x64 -p:PlatformToolset=v145 -v:minimal
if errorlevel 1 (
    popd & exit /b 1
)
set "DLL=build\x64\Release\foo_tealscrobbler.dll"
set "PACKAGE=build\x64\Release\foo_tealscrobbler.fb2k-component"
call "%~dp0package_component.cmd" "%DLL%" "%PACKAGE%"
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
