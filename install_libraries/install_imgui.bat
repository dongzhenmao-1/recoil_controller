@echo off
setlocal EnableDelayedExpansion

:: 动态解析上一级目录（项目根目录）
for %%i in ("%~dp0..") do set "PROJECT_ROOT=%%~fi"

set "TRIPLET=x64-windows"
set "VCPKG_DIR=%PROJECT_ROOT%\build\vcpkg_installed\%TRIPLET%"
set "TEMP_DIR=%TEMP%\imgui_tmp"
set "ZIP_URL=https://github.com/ocornut/imgui/archive/refs/tags/v1.92.8.zip"
set "ZIP_FILE=%TEMP_DIR%\imgui.zip"
set "PS1_FILE=%TEMP_DIR%\imgui_install.ps1"

echo.
echo ========================================
echo   Dear ImGui v1.92.8 Installer [%TRIPLET%]
echo ========================================
echo   Target: %VCPKG_DIR%
echo.

if exist "%VCPKG_DIR%\include\imgui\imgui.h" (
    echo [SKIP] ImGui is already installed.
    goto :end_ok
)

if not exist "%TEMP_DIR%" mkdir "%TEMP_DIR%"

:: 使用逐行写入(>>)，并显式使用 .FullName 提取绝对路径，彻底解决路径丢失问题
echo $ErrorActionPreference = 'Stop' > "%PS1_FILE%"
echo [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 >> "%PS1_FILE%"
echo $zip = '%ZIP_FILE%' >> "%PS1_FILE%"
echo $out = '%TEMP_DIR%\extracted' >> "%PS1_FILE%"
echo $dst = '%VCPKG_DIR%\include\imgui' >> "%PS1_FILE%"
echo Write-Host '[1/3] Downloading ImGui source zip...' >> "%PS1_FILE%"
echo (New-Object Net.WebClient).DownloadFile('%ZIP_URL%', $zip) >> "%PS1_FILE%"
echo Write-Host '[2/3] Extracting...' >> "%PS1_FILE%"
echo Expand-Archive -LiteralPath $zip -DestinationPath $out -Force >> "%PS1_FILE%"
echo Write-Host '[3/3] Organizing ImGui files...' >> "%PS1_FILE%"
echo New-Item -ItemType Directory -Force -Path "$dst" ^| Out-Null >> "%PS1_FILE%"
echo New-Item -ItemType Directory -Force -Path "$dst\backends" ^| Out-Null >> "%PS1_FILE%"
echo # 核心修复：显式使用 .FullName 属性锁定绝对路径
echo $imgui_src = (Get-ChildItem $out -Directory ^| Select-Object -First 1).FullName >> "%PS1_FILE%"
echo Copy-Item -Path "$imgui_src\*.h" -Destination "$dst\" -Force >> "%PS1_FILE%"
echo Copy-Item -Path "$imgui_src\*.cpp" -Destination "$dst\" -Force >> "%PS1_FILE%"
echo Copy-Item -Path "$imgui_src\backends\*" -Destination "$dst\backends\" -Recurse -Force >> "%PS1_FILE%"
echo Write-Host 'Done.' >> "%PS1_FILE%"

powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1_FILE%"

if errorlevel 1 (
    echo.
    echo [ERROR] Installation failed.
    goto :end_fail
)

rd /s /q "%TEMP_DIR%" 2>nul
echo.
echo Dear ImGui installed successfully into "%VCPKG_DIR%\include\imgui"!
:end_ok
pause
exit /b 0

:end_fail
pause
exit /b 1