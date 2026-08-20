@echo off

rem Copyright (c) Wojciech Figat. All rights reserved.

setlocal
pushd

echo Rebuilding Flax Editor (Development)...

cd /D "%~dp0"

tasklist /FI "IMAGENAME eq FlaxEditor.exe" 2>nul | find /I "FlaxEditor.exe" >nul
if errorlevel 1 goto Build

echo FlaxEditor.exe is running. Closing it so Development binaries can be overwritten...
taskkill /IM FlaxEditor.exe >nul 2>nul

set WaitCount=0
:WaitEditor
tasklist /FI "IMAGENAME eq FlaxEditor.exe" 2>nul | find /I "FlaxEditor.exe" >nul
if errorlevel 1 goto Build
set /A WaitCount+=1
if %WaitCount% GEQ 30 goto EditorStillRunning
timeout /t 2 /nobreak >nul
goto WaitEditor

:Build
call "Development\Scripts\Windows\CallBuildTool.bat" -build -log -dotnet=8 -arch=x64 -platform=Windows -configuration=Development -buildtargets=FlaxEditor %*
if errorlevel 1 goto BuildToolFailed

popd
echo Done!
exit /B 0

:EditorStillRunning
echo FlaxEditor.exe is still running. Close it and run this script again.
goto Exit

:BuildToolFailed
echo Flax.Build tool failed.
goto Exit

:Exit
popd
exit /B 1
