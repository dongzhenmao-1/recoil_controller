@echo off
setlocal EnableDelayedExpansion

:: 动态解析上一级目录（项目根目录）
for %%i in ("%~dp0..") do set "PROJECT_ROOT=%%~fi"

set "TRIPLET=x64-windows"
set "VCPKG_DIR=%PROJECT_ROOT%\build\vcpkg_installed\%TRIPLET%"
set "TEMP_DIR=%TEMP%\ort_gpu_tmp"
:: 下载官方 ONNX Runtime 1.20.0 GPU Windows x64 预编译包 (约 210MB)
set "ZIP_URL=https://github.com/microsoft/onnxruntime/releases/download/v1.20.0/onnxruntime-win-x64-gpu-1.20.0.zip"
set "ZIP_FILE=%TEMP_DIR%\onnxruntime_gpu.zip"
set "PS1_FILE=%TEMP_DIR%\ort_gpu_install.ps1"

echo.
echo ========================================
echo   ONNX Runtime GPU Installer [%TRIPLET%]
echo ========================================
echo   Target: %VCPKG_DIR%
echo.

if exist "%VCPKG_DIR%\include\onnxruntime_cxx_api.h" (
    echo [INFO] Detected existing ONNX Runtime headers.
    echo        Replacing it with GPU-enabled version...
)

if not exist "%TEMP_DIR%" mkdir "%TEMP_DIR%"

:: 使用逐行写入(>>)，并显式使用 .FullName 提取绝对路径，彻底解决路径丢失问题
echo $ErrorActionPreference = 'Stop' > "%PS1_FILE%"
echo [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 >> "%PS1_FILE%"
echo $zip = '%ZIP_FILE%' >> "%PS1_FILE%"
echo $out = '%TEMP_DIR%\extracted' >> "%PS1_FILE%"
echo $dst = '%VCPKG_DIR%' >> "%PS1_FILE%"
echo Write-Host '[1/3] Downloading ONNX Runtime GPU (approx 210MB)...' >> "%PS1_FILE%"
echo (New-Object Net.WebClient).DownloadFile('%ZIP_URL%', $zip) >> "%PS1_FILE%"
echo Write-Host '[2/3] Extracting...' >> "%PS1_FILE%"
echo Expand-Archive -LiteralPath $zip -DestinationPath $out -Force >> "%PS1_FILE%"
echo Write-Host '[3/3] Reorganizing and copying GPU files...' >> "%PS1_FILE%"
echo New-Item -ItemType Directory -Force -Path "$dst\include","$dst\lib","$dst\bin","$dst\debug\lib","$dst\debug\bin" ^| Out-Null >> "%PS1_FILE%"
echo $ort_dir = (Get-ChildItem $out -Directory ^| Select-Object -First 1).FullName >> "%PS1_FILE%"
echo Copy-Item -Path "$ort_dir\include\*" -Destination "$dst\include\" -Recurse -Force >> "%PS1_FILE%"
echo Copy-Item -Path "$ort_dir\lib\*.lib" -Destination "$dst\lib\" -Force >> "%PS1_FILE%"
echo Copy-Item -Path "$ort_dir\lib\*.lib" -Destination "$dst\debug\lib\" -Force >> "%PS1_FILE%"
echo # 自动拷贝该目录下所有的 .dll 文件（包括 cuda 支撑动态库）
echo Copy-Item -Path "$ort_dir\lib\*.dll" -Destination "$dst\bin\" -Force >> "%PS1_FILE%"
echo Copy-Item -Path "$ort_dir\lib\*.dll" -Destination "$dst\debug\bin\" -Force >> "%PS1_FILE%"
echo Write-Host 'Done.' >> "%PS1_FILE%"

powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1_FILE%"

if errorlevel 1 (
    echo.
    echo [ERROR] Installation failed.
    goto :end_fail
)

rd /s /q "%TEMP_DIR%" 2>nul
echo.
echo ONNX Runtime GPU installed successfully into "%VCPKG_DIR%"!
:end_ok
pause
exit /b 0

:end_fail
pause
exit /b 1