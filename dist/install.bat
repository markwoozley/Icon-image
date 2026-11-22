@echo off
setlocal
rem Installs the CAD thumbnail handler for the current user (no admin needed).
set "SRC=%~dp0CadThumbnails.dll"
set "DEST=%LOCALAPPDATA%\CadThumbnails"
set "REGSVR=%SystemRoot%\System32\regsvr32.exe"
if exist "%SystemRoot%\Sysnative\regsvr32.exe" set "REGSVR=%SystemRoot%\Sysnative\regsvr32.exe"

if not exist "%SRC%" (
  echo Could not find CadThumbnails.dll next to this script.
  pause
  exit /b 1
)

if exist "%DEST%\CadThumbnails.dll" "%REGSVR%" /s /u "%DEST%\CadThumbnails.dll"
if not exist "%DEST%" mkdir "%DEST%"
copy /y "%SRC%" "%DEST%\CadThumbnails.dll" >nul 2>&1
if errorlevel 1 (
  rem The old DLL is still loaded by the thumbnail host process; stop it and retry.
  taskkill /f /im dllhost.exe >nul 2>&1
  timeout /t 2 /nobreak >nul
  copy /y "%SRC%" "%DEST%\CadThumbnails.dll" >nul
)
if errorlevel 1 (
  echo Could not copy the DLL. If Explorer is using an older copy, sign out and back in, then try again.
  pause
  exit /b 1
)
rem Remove the "downloaded from the internet" mark so Windows will load it.
powershell -NoProfile -Command "Unblock-File -Path '%DEST%\CadThumbnails.dll'" >nul 2>&1

"%REGSVR%" /s "%DEST%\CadThumbnails.dll"
if errorlevel 1 (
  echo Registration failed.
  pause
  exit /b 1
)
echo.
echo Installed (v3). Thumbnails are now enabled for:
echo   .sldprt .sldasm .slddrw  (SolidWorks)
echo   .step .stp               (STEP)
echo   .stl                     (STL)
echo.
echo In File Explorer choose View ^> Medium/Large icons to see them.
echo.
choice /m "Restart File Explorer now so the change takes effect immediately"
if errorlevel 2 goto :eof
taskkill /f /im explorer.exe >nul 2>&1
del /f /q "%LOCALAPPDATA%\Microsoft\Windows\Explorer\thumbcache_*.db" >nul 2>&1
start explorer.exe
