@echo off
setlocal
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo Visual Studio with the C++ workload was not found. & exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
pushd "%~dp0.."
cl /nologo /std:c++20 /EHsc /MD /O2 /W4 /utf-8 /I vendor test\test_teal_client.cpp src\teal_client.cpp src\teal_did.cpp src\teal_records.cpp src\teal_time.cpp src\play_queue.cpp /Fe:test\test_teal_client.exe
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
