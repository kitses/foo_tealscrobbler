@echo off
setlocal
rem Assemble an installable .fb2k-component archive from a built component DLL.
if "%~2"=="" (
    echo usage: package_component.cmd ^<component.dll^> ^<output.fb2k-component^> & exit /b 2
)

for %%i in ("%~dp0..") do set "ROOT=%%~fi"
set "DLL=%~f1"
set "OUT=%~f2"
set "STAGE=%ROOT%\build\package"

if not exist "%DLL%" (
    echo Component DLL not found: %DLL% & exit /b 1
)

if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%" || exit /b 1
copy /y "%DLL%" "%STAGE%\" >nul || exit /b 1
copy /y "%ROOT%\LICENSE" "%STAGE%\LICENSE" >nul || exit /b 1

if exist "%OUT%" del /q "%OUT%"
powershell -NoProfile -ExecutionPolicy Bypass -Command "Add-Type -AssemblyName System.IO.Compression.FileSystem; [System.IO.Compression.ZipFile]::CreateFromDirectory('%STAGE%','%OUT%',[System.IO.Compression.CompressionLevel]::Optimal,$false)"
if errorlevel 1 exit /b 1

echo Component package: %OUT%
exit /b 0
