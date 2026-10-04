@echo off
rem Flash the Spectr Node factory image to an ESP32-S3 over USB.
rem
rem   flash-spectr.bat [COM_PORT] [FACTORY_IMAGE]
rem
rem Defaults: COM4 and the newest firmware-out\spectr-node-*-factory.bin.
rem This is the ONLY step that writes to a device; run it only when you decide to flash.
setlocal enabledelayedexpansion
set PORT=%~1
if "%PORT%"=="" set PORT=COM4
set IMAGE=%~2
if not "%IMAGE%"=="" goto flash

for /f "delims=" %%f in ('dir /b /o-d "firmware-out\spectr-node-*-factory.bin" 2^>nul') do (
  set "IMAGE=firmware-out\%%f"
  goto flash
)
echo No factory image found in firmware-out\.
echo Build first:  bash scripts/build-firmware.sh   (on the VPS)
exit /b 1

:flash
echo Flashing "%IMAGE%" to %PORT% ...
esptool.py --chip esp32s3 --port %PORT% write_flash 0x0 "%IMAGE%"
if errorlevel 1 (
  echo.
  echo Flash failed. Check the port, close any serial monitor, and try again.
  exit /b 1
)
echo.
echo Done. The sensor boots into setup mode if it has no saved Wi-Fi.
endlocal
