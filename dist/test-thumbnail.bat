@echo off
setlocal
rem Drag a .sldprt/.sldasm/.step/.stp/.stl file onto this script to test the
rem thumbnail handler. Results go to a temporary folder (not next to your file).
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
set "OUTDIR=%TEMP%\CadThumbnailTest"
if not exist "%OUTDIR%" mkdir "%OUTDIR%"
set "OUT=%OUTDIR%\%~n1.thumb.bmp"
"%~dp0wintest.exe" "%DLL%" "%~f1" "%OUT%" 256
if errorlevel 1 (
  echo.
  echo TEST FAILED - please send a photo of this window.
) else (
  echo.
  echo Thumbnail saved in %OUTDIR% - opening it.
  start "" "%OUT%"
)
pause
