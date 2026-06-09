@echo off
setlocal EnableDelayedExpansion

:: 动态解析上一级目录（项目根目录）
for %%i in ("%~dp0..") do set "PROJECT_ROOT=%%~fi"

set "TRIPLET=x64-windows"
set "VCPKG_DIR=%PROJECT_ROOT%\build\vcpkg_installed\%TRIPLET%"
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

:: 使用逐行写入(>>)，避免批处理括号解析错误
echo $ErrorActionPreference = 'Stop' > "%PS1_FILE%"
echo [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 >> "%PS1_FILE%"
echo $zip = '%ZIP_FILE%' >> "%PS1_FILE%"
echo $out = '%TEMP_DIR%\extracted' >> "%PS1_FILE%"
echo $dst = '%VCPKG_DIR%' >> "%PS1_FILE%"
echo Write-Host '[1/3] Downloading...' >> "%PS1_FILE%"
echo (New-Object Net.WebClient).DownloadFile('%ZIP_URL%', $zip) >> "%PS1_FILE%"
echo Write-Host '[2/3] Extracting...' >> "%PS1_FILE%"
echo Expand-Archive -LiteralPath $zip -DestinationPath $out -Force >> "%PS1_FILE%"
echo Write-Host '[3/3] Copying files...' >> "%PS1_FILE%"
echo New-Item -ItemType Directory -Force -Path "$dst\include","$dst\lib","$dst\bin","$dst\debug\lib","$dst\debug\bin","$dst\share\interception","$dst\tools\interception" ^| Out-Null >> "%PS1_FILE%"
echo Get-ChildItem $out -Recurse -Filter 'interception.h' ^| Select-Object -First 1 ^| Copy-Item -Destination "$dst\include\" -Force >> "%PS1_FILE%"
echo Get-ChildItem $out -Recurse -Filter 'interception.lib' ^| Where-Object { $_.FullName -match 'x64' } ^| Copy-Item -Destination "$dst\lib\" -Force >> "%PS1_FILE%"
echo Get-ChildItem $out -Recurse -Filter 'interception.lib' ^| Where-Object { $_.FullName -match 'x64' } ^| Copy-Item -Destination "$dst\debug\lib\" -Force >> "%PS1_FILE%"
echo Get-ChildItem $out -Recurse -Filter 'interception.dll' ^| Where-Object { $_.FullName -match 'x64' } ^| Copy-Item -Destination "$dst\bin\" -Force >> "%PS1_FILE%"
echo Get-ChildItem $out -Recurse -Filter 'interception.dll' ^| Where-Object { $_.FullName -match 'x64' } ^| Copy-Item -Destination "$dst\debug\bin\" -Force >> "%PS1_FILE%"
echo Get-ChildItem $out -Recurse -Filter 'install-interception.exe' ^| Select-Object -First 1 ^| Copy-Item -Destination "$dst\tools\interception\" -Force >> "%PS1_FILE%"
echo Write-Host 'Done.' >> "%PS1_FILE%"

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

:ask_driver
echo ========================================
echo   Driver Install (needs Admin + reboot^)
echo ========================================
set /p "INSTALL_DRV=Install kernel driver now? (y/N): "
if /i "!INSTALL_DRV!" NEQ "y" (
    echo Skipped. Run manually later.
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