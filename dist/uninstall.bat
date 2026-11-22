@echo off
setlocal
set "DEST=%LOCALAPPDATA%\CadThumbnails"
set "REGSVR=%SystemRoot%\System32\regsvr32.exe"
if exist "%SystemRoot%\Sysnative\regsvr32.exe" set "REGSVR=%SystemRoot%\Sysnative\regsvr32.exe"
if exist "%DEST%\CadThumbnails.dll" "%REGSVR%" /s /u "%DEST%\CadThumbnails.dll"
echo Thumbnail handler unregistered.
choice /m "Restart File Explorer now (needed before the DLL file can be deleted)"
if errorlevel 2 goto :eof
taskkill /f /im explorer.exe >nul 2>&1
del /f /q "%LOCALAPPDATA%\Microsoft\Windows\Explorer\thumbcache_*.db" >nul 2>&1
timeout /t 2 /nobreak >nul
rmdir /s /q "%DEST%" >nul 2>&1
start explorer.exe
