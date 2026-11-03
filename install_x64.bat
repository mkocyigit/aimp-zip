@echo off
REM AIMP 64-bit icin eklentiyi kurar. YONETICI olarak calistir.
setlocal
set SRC=%~dp0build-x64\bin\Release\aimp_zip.dll
set DST=C:\Program Files\AIMP\Plugins\aimp_zip

if not exist "%SRC%" (
  echo HATA: %SRC% yok. Once build_vs2026_x64.bat calistir.
  pause & exit /b 1
)

tasklist /FI "IMAGENAME eq AIMP.exe" | find /I "AIMP.exe" >nul
if not errorlevel 1 (
  echo HATA: AIMP acik. Once kapat.
  pause & exit /b 1
)

if not exist "%DST%" mkdir "%DST%"
copy /Y "%SRC%" "%DST%\aimp_zip.dll"
if errorlevel 1 (
  echo HATA: kopyalanamadi - bu dosyayi YONETICI olarak calistirdigindan emin ol.
  pause & exit /b 1
)

echo TAMAM: 64-bit aimp_zip.dll kuruldu.
pause
