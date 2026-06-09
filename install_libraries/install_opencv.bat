@echo off
setlocal EnableDelayedExpansion

:: 动态解析上一级目录（项目根目录）
for %%i in ("%~dp0..") do set "PROJECT_ROOT=%%~fi"

set "TRIPLET=x64-windows"
set "VCPKG_DIR=%PROJECT_ROOT%\build\vcpkg_installed\%TRIPLET%"
set "TEMP_DIR=%TEMP%\opencv_tmp"
set "ZIP_URL=https://github.com/opencv/opencv/releases/download/4.10.0/opencv-4.10.0-windows.exe"
set "ZIP_FILE=%TEMP_DIR%\opencv.exe"
set "PS1_FILE=%TEMP_DIR%\opencv_install.ps1"

echo.
echo ========================================
echo   OpenCV 4.10.0 Installer  [%TRIPLET%]
echo ========================================
echo   Target: %VCPKG_DIR%
echo.

if exist "%VCPKG_DIR%\include\opencv2\opencv.hpp" (
    echo [SKIP] OpenCV is already installed.
    goto :end_ok
)

if not exist "%TEMP_DIR%" mkdir "%TEMP_DIR%"

:: 使用逐行写入(>>)，避免批处理括号解析错误
echo $ErrorActionPreference = 'Stop' > "%PS1_FILE%"
echo [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 >> "%PS1_FILE%"
echo $exe = '%ZIP_FILE%' >> "%PS1_FILE%"
echo $out = '%TEMP_DIR%\extracted' >> "%PS1_FILE%"
echo $dst = '%VCPKG_DIR%' >> "%PS1_FILE%"
echo Write-Host '[1/3] Downloading OpenCV (approx 180MB)...' >> "%PS1_FILE%"
echo (New-Object Net.WebClient).DownloadFile('%ZIP_URL%', $exe) >> "%PS1_FILE%"
echo Write-Host '[2/3] Extracting OpenCV...' >> "%PS1_FILE%"
echo Start-Process -FilePath $exe -ArgumentList "-o`"$out`" -y" -Wait -NoNewWindow >> "%PS1_FILE%"
echo Write-Host '[3/3] Reorganizing and copying files...' >> "%PS1_FILE%"
echo New-Item -ItemType Directory -Force -Path "$dst\include","$dst\lib","$dst\bin","$dst\debug\lib","$dst\debug\bin" ^| Out-Null >> "%PS1_FILE%"
echo Copy-Item -Path "$out\opencv\build\include\*" -Destination "$dst\include\" -Recurse -Force >> "%PS1_FILE%"
echo $vc_dir = Get-ChildItem "$out\opencv\build\x64" -Directory ^| Where-Object { $_.Name -match 'vc\d+' } ^| Select-Object -First 1 >> "%PS1_FILE%"
echo Get-ChildItem $vc_dir.FullName -Recurse -Filter 'opencv_world*.lib' ^| Where-Object { $_.Name -notmatch 'd\.lib$' } ^| Copy-Item -Destination "$dst\lib\" -Force >> "%PS1_FILE%"
echo Get-ChildItem $vc_dir.FullName -Recurse -Filter 'opencv_world*d.lib' ^| Copy-Item -Destination "$dst\debug\lib\" -Force >> "%PS1_FILE%"
echo Get-ChildItem $vc_dir.FullName -Recurse -Filter 'opencv_world*.dll' ^| Where-Object { $_.Name -notmatch 'd\.dll$' } ^| Copy-Item -Destination "$dst\bin\" -Force >> "%PS1_FILE%"
echo Get-ChildItem $vc_dir.FullName -Recurse -Filter 'opencv_world*d.dll' ^| Copy-Item -Destination "$dst\debug\bin\" -Force >> "%PS1_FILE%"
echo Write-Host 'Done.' >> "%PS1_FILE%"

powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1_FILE%"

if errorlevel 1 (
    echo.
    echo [ERROR] Installation failed.
    goto :end_fail
)

rd /s /q "%TEMP_DIR%" 2>nul
echo.
echo OpenCV installed successfully into "%VCPKG_DIR%"!
:end_ok
pause
exit /b 0

:end_fail
pause
exit /b 1