@echo off
setlocal EnableDelayedExpansion

:: 动态解析上一级目录（项目根目录）
for %%i in ("%~dp0..") do set "PROJECT_ROOT=%%~fi"

set "TRIPLET=x64-windows"
set "VCPKG_DIR=%PROJECT_ROOT%\build\vcpkg_installed\%TRIPLET%"
set "TEMP_DIR=%TEMP%\interception_uninstall_tmp"
set "ZIP_URL=https://github.com/oblitum/Interception/releases/download/v1.0.1/Interception.zip"
set "ZIP_FILE=%TEMP_DIR%\Interception.zip"
set "PS1_FILE=%TEMP_DIR%\ic_uninstall.ps1"

set "DEL_INCLUDE=%VCPKG_DIR%\include\interception.h"
set "DEL_LIB_X64=%VCPKG_DIR%\lib\interception.lib"
set "DEL_DLIB_X64=%VCPKG_DIR%\debug\lib\interception.lib"
set "DEL_BIN_X64=%VCPKG_DIR%\bin\interception.dll"
set "DEL_DBIN_X64=%VCPKG_DIR%\debug\bin\interception.dll"
set "DEL_TOOLS=%VCPKG_DIR%\tools\interception"

echo.
echo ========================================
echo   Interception Uninstaller
echo ========================================
echo.

if not exist "%TEMP_DIR%" mkdir "%TEMP_DIR%"

:: 1. 写入临时 PS1 变量定义（顺序写入，无嵌套，避免批处理解析冲突）
echo $ErrorActionPreference = 'Stop' > "%PS1_FILE%"
echo [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 >> "%PS1_FILE%"
echo $local_exe = '%VCPKG_DIR%\tools\interception\install-interception.exe' >> "%PS1_FILE%"
echo $zip_url = '%ZIP_URL%' >> "%PS1_FILE%"
echo $zip_file = '%ZIP_FILE%' >> "%PS1_FILE%"
echo $temp_dir = '%TEMP_DIR%' >> "%PS1_FILE%"

:: 2. 写入 PowerShell 中的判断与下载逻辑（由 PS 解析，规避 CMD 括号缺陷）
echo if ^(Test-Path $local_exe^) { >> "%PS1_FILE%"
echo     Write-Host '[OK] Found local uninstaller.' >> "%PS1_FILE%"
echo     $drv_exe = $local_exe >> "%PS1_FILE%"
echo } else { >> "%PS1_FILE%"
echo     Write-Host '[INFO] Local uninstaller not found. Downloading temp package for uninstallation...' >> "%PS1_FILE%"
echo     $out = Join-Path $temp_dir 'extracted' >> "%PS1_FILE%"
echo     Invoke-WebRequest -UseBasicParsing -Uri $zip_url -OutFile $zip_file >> "%PS1_FILE%"
echo     Expand-Archive -LiteralPath $zip_file -DestinationPath $out -Force >> "%PS1_FILE%"
echo     $found = Get-ChildItem $out -Recurse -Filter 'install-interception.exe' ^| Select-Object -First 1 >> "%PS1_FILE%"
echo     $drv_exe = $found.FullName >> "%PS1_FILE%"
echo } >> "%PS1_FILE%"

:: 3. 写入卸载及清理命令
echo Write-Host 'Uninstalling kernel driver (requires Admin permission)...' >> "%PS1_FILE%"
echo Start-Process $drv_exe -ArgumentList '/uninstall' -Verb RunAs -Wait >> "%PS1_FILE%"

echo Write-Host 'Driver uninstalled. Cleaning up project files...' >> "%PS1_FILE%"
echo Remove-Item -Path '%DEL_INCLUDE%' -ErrorAction SilentlyContinue >> "%PS1_FILE%"
echo Remove-Item -Path '%DEL_LIB_X64%' -ErrorAction SilentlyContinue >> "%PS1_FILE%"
echo Remove-Item -Path '%DEL_DLIB_X64%' -ErrorAction SilentlyContinue >> "%PS1_FILE%"
echo Remove-Item -Path '%DEL_BIN_X64%' -ErrorAction SilentlyContinue >> "%PS1_FILE%"
echo Remove-Item -Path '%DEL_DBIN_X64%' -ErrorAction SilentlyContinue >> "%PS1_FILE%"
echo Remove-Item -Path '%DEL_TOOLS%' -Recurse -ErrorAction SilentlyContinue >> "%PS1_FILE%"
echo Write-Host 'Cleanup completed successfully.' >> "%PS1_FILE%"

:: 4. 执行生成好的 PowerShell 脚本
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1_FILE%"

if errorlevel 1 (
    echo.
    echo [ERROR] Uninstallation or cleanup failed.
    goto :end_fail
)

:: 清理临时文件夹
rd /s /q "%TEMP_DIR%" 2>nul

echo.
echo ========================================
echo   Uninstalled successfully!
echo   Please REBOOT your PC to apply changes.
echo ========================================
echo.

:end_ok
pause
exit /b 0

:end_fail
rd /s /q "%TEMP_DIR%" 2>nul
pause
exit /b 1