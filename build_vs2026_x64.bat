@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b %errorlevel%
cmake -S . -B build-x64 -A x64
if errorlevel 1 exit /b %errorlevel%
cmake --build build-x64 --config Release
exit /b %errorlevel%
