@echo off
setlocal EnableDelayedExpansion

:: ============================================================
::  install_interception.bat
::  Installs Interception into build\vcpkg_installed\x64-windows\
::  matching the layout produced by CMake + vcpkg.json.
:: ============================================================

set "TRIPLET=x64-windows"
set "VCPKG_DIR=%~dp0build\vcpkg_installed\%TRIPLET%"
set "TEMP_DIR=%TEMP%\interception_tmp"
set "ZIP_URL=https://github.com/oblitum/Interception/releases/download/v1.0.1/Interception.zip"
set "ZIP_FILE=%TEMP_DIR%\Interception.zip"
set "PS1_FILE=%TEMP_DIR%\ic_install.ps1"

echo.
echo ========================================
echo   Interception Installer  [%TRIPLET%]
echo ========================================
echo   Target: %VCPKG_DIR%
echo.

if exist "%VCPKG_DIR%\include\interception.h" (
    echo [SKIP] Already installed.
    echo        Delete build\vcpkg_installed\ to reinstall.
    goto :ask_driver
)

if not exist "%TEMP_DIR%" mkdir "%TEMP_DIR%"

(
echo $ErrorActionPreference = 'Stop'
echo [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
echo $zip = '%ZIP_FILE%'
echo $out = '%TEMP_DIR%\extracted'
echo $dst = '%VCPKG_DIR%'
echo Write-Host '[1/3] Downloading...'
echo (New-Object Net.WebClient^).DownloadFile('%ZIP_URL%', $zip^)
echo Write-Host '[2/3] Extracting...'
echo Expand-Archive -LiteralPath $zip -DestinationPath $out -Force
echo Write-Host '[3/3] Copying files...'
echo New-Item -ItemType Directory -Force -Path "$dst\include","$dst\lib","$dst\bin","$dst\debug\lib","$dst\debug\bin","$dst\share\interception","$dst\tools\interception" ^| Out-Null
echo Get-ChildItem $out -Recurse -Filter 'interception.h' ^| Select-Object -First 1 ^| Copy-Item -Destination "$dst\include\" -Force
echo # Copy x64 .lib to both release and debug folders to avoid architecture mismatch (x86 vs x64^)
echo Get-ChildItem $out -Recurse -Filter 'interception.lib' ^| Where-Object { $_.FullName -match 'x64' } ^| Copy-Item -Destination "$dst\lib\" -Force
echo Get-ChildItem $out -Recurse -Filter 'interception.lib' ^| Where-Object { $_.FullName -match 'x64' } ^| Copy-Item -Destination "$dst\debug\lib\" -Force
echo # Copy x64 .dll to bin and debug\bin for runtime execution
echo Get-ChildItem $out -Recurse -Filter 'interception.dll' ^| Where-Object { $_.FullName -match 'x64' } ^| Copy-Item -Destination "$dst\bin\" -Force
echo Get-ChildItem $out -Recurse -Filter 'interception.dll' ^| Where-Object { $_.FullName -match 'x64' } ^| Copy-Item -Destination "$dst\debug\bin\" -Force
echo Get-ChildItem $out -Recurse -Filter 'install-interception.exe' ^| Select-Object -First 1 ^| Copy-Item -Destination "$dst\tools\interception\" -Force
echo Write-Host 'Done.'
) > "%PS1_FILE%"

powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1_FILE%"

if errorlevel 1 (
    echo.
    echo [ERROR] Installation failed. Check your network connection.
    echo         Manual download: %ZIP_URL%
    goto :end_fail
)

rd /s /q "%TEMP_DIR%" 2>nul

echo.
echo ========================================
echo   Installed successfully!
echo ========================================
echo.
echo   build\vcpkg_installed\%TRIPLET%\
echo     include\interception.h
echo     lib\interception.lib (x64 Release^)
echo     bin\interception.dll (x64 Release^)
echo     debug\lib\interception.lib (x64 Debug^)
echo     debug\bin\interception.dll (x64 Debug^)
echo     tools\interception\install-interception.exe
echo.
echo   [WARNING] If you modify vcpkg.json or clean your CMake build,
echo             vcpkg may wipe the "vcpkg_installed" folder.
echo             In that case, you will need to re-run this script.
echo.
echo CMakeLists.txt usage:
echo   find_library(INTERCEPTION_LIB interception
echo       PATHS "${CMAKE_BINARY_DIR}/vcpkg_installed/%TRIPLET%/lib"^)
echo   target_link_libraries(your_target PRIVATE ${INTERCEPTION_LIB}^)
echo   target_include_directories(your_target PRIVATE
echo       "${CMAKE_BINARY_DIR}/vcpkg_installed/%TRIPLET%/include"^)
echo.

:ask_driver
echo ========================================
echo   Driver Install (needs Admin + reboot^)
echo ========================================
set /p "INSTALL_DRV=Install kernel driver now? (y/N): "
if /i "!INSTALL_DRV!" NEQ "y" (
    echo Skipped. Run manually later:
    echo   build\vcpkg_installed\%TRIPLET%\tools\interception\install-interception.exe /install
    goto :end_ok
)

set "DRV_EXE=%VCPKG_DIR%\tools\interception\install-interception.exe"
if not exist "%DRV_EXE%" (
    echo [ERROR] Driver not found: %DRV_EXE%
    goto :end_ok
)
powershell -Command "Start-Process '%DRV_EXE%' -ArgumentList '/install' -Verb RunAs -Wait"
echo [OK] Driver installed. Please reboot your PC.

:end_ok
echo.
echo All done!
pause
exit /b 0

:end_fail
echo.
pause
exit /b 1