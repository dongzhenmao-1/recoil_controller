@echo off
setlocal EnableDelayedExpansion

:: 动态解析上一级目录（项目根目录）
for %%i in ("%~dp0..") do set "PROJECT_ROOT=%%~fi"

set "TRIPLET=x64-windows"
set "VCPKG_DIR=%PROJECT_ROOT%\build\vcpkg_installed\%TRIPLET%"
set "TEMP_DIR=%TEMP%\json_tmp"
:: 下载最新稳定版 v3.12.0 的单头文件 json.hpp
set "ZIP_URL=https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp"
set "ZIP_FILE=%TEMP_DIR%\json.hpp"
set "PS1_FILE=%TEMP_DIR%\json_install.ps1"

echo.
echo ========================================
echo   nlohmann/json Installer  [%TRIPLET%]
echo ========================================
echo   Target: %VCPKG_DIR%
echo.

if exist "%VCPKG_DIR%\include\nlohmann\json.hpp" (
    echo [SKIP] nlohmann/json is already installed.
    goto :end_ok
)

if not exist "%TEMP_DIR%" mkdir "%TEMP_DIR%"

:: 使用逐行写入(>>)，避免批处理括号解析错误
echo $ErrorActionPreference = 'Stop' > "%PS1_FILE%"
echo [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 >> "%PS1_FILE%"
echo $url = '%ZIP_URL%' >> "%PS1_FILE%"
echo $file = '%ZIP_FILE%' >> "%PS1_FILE%"
echo $dst = '%VCPKG_DIR%' >> "%PS1_FILE%"
echo Write-Host '[1/2] Downloading json.hpp...' >> "%PS1_FILE%"
echo (New-Object Net.WebClient).DownloadFile($url, $file) >> "%PS1_FILE%"
echo Write-Host '[2/2] Organizing header files...' >> "%PS1_FILE%"
echo New-Item -ItemType Directory -Force -Path "$dst\include\nlohmann" ^| Out-Null >> "%PS1_FILE%"
echo Copy-Item -Path $file -Destination "$dst\include\nlohmann\" -Force >> "%PS1_FILE%"
echo Write-Host 'Done.' >> "%PS1_FILE%"

powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1_FILE%"

if errorlevel 1 (
    echo.
    echo [ERROR] Installation failed.
    goto :end_fail
)

rd /s /q "%TEMP_DIR%" 2>nul
echo.
echo nlohmann/json installed successfully into "%VCPKG_DIR%"!
:end_ok
pause
exit /b 0

:end_fail
pause
exit /b 1