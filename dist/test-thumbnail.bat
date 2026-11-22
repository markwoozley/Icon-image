@echo off
setlocal
rem Drag a .sldprt/.sldasm/.step/.stp/.stl file onto this script to test the
rem thumbnail handler. It saves the result next to the file as <name>.thumb.bmp
rem and opens it.
if "%~1"=="" (
  echo Drag a CAD file onto this script to test it.
  pause
  exit /b 1
)
set "DLL=%LOCALAPPDATA%\CadThumbnails\CadThumbnails.dll"
if not exist "%DLL%" (
  echo The handler is not installed yet - run install.bat first.
  pause
  exit /b 1
)
set "OUT=%~dpn1.thumb.bmp"
"%~dp0wintest.exe" "%DLL%" "%~f1" "%OUT%" 256
if errorlevel 1 (
  echo.
  echo TEST FAILED - please copy the lines above and send them back.
) else (
  echo.
  echo Success - opening %OUT%
  start "" "%OUT%"
)
pause
