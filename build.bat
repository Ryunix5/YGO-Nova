@echo off
setlocal

REM ── EdoPro+ Windows Build Script ────────────────────────────────────────────
REM Always operate relative to the directory this script lives in,
REM regardless of where the caller's working directory is.
cd /d "%~dp0"
REM Prerequisites:
REM   - Visual Studio 2022 (with C++ workload)
REM   - CMake 3.20+
REM   - vcpkg installed and VCPKG_ROOT env set
REM   - Git (for cloning imgui/stb if not already present)

set BUILD_DIR=build\windows

REM ── Fetch vendor dependencies if missing ────────────────────────────────────
if not exist vendor\imgui\imgui.h (
    echo Cloning Dear ImGui...
    git clone --depth 1 https://github.com/ocornut/imgui.git vendor\imgui
)
if not exist vendor\stb\stb_image.h (
    echo Downloading stb_image...
    mkdir vendor\stb 2>nul
    curl -L -o vendor\stb\stb_image.h https://raw.githubusercontent.com/nothings/stb/master/stb_image.h
)

REM ── Create build dir ────────────────────────────────────────────────────────
mkdir %BUILD_DIR% 2>nul
cd %BUILD_DIR%

REM ── Check vcpkg ─────────────────────────────────────────────────────────────
if not defined VCPKG_ROOT (
    echo ERROR: VCPKG_ROOT is not set.
    echo Run these commands first:
    echo   git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
    echo   C:\vcpkg\bootstrap-vcpkg.bat
    echo   setx VCPKG_ROOT C:\vcpkg
    echo Then open a NEW terminal and run build.bat again.
    pause
    exit /b 1
)

REM ── Configure ───────────────────────────────────────────────────────────────
echo Configuring...
cmake ..\.. ^
    -G "Visual Studio 17 2022" ^
    -A x64 ^
    -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake ^
    -DVCPKG_TARGET_TRIPLET=x64-windows-static-md ^
    -DCMAKE_BUILD_TYPE=Release

if errorlevel 1 (
    echo CMake configure failed.
    exit /b 1
)

REM ── Build ───────────────────────────────────────────────────────────────────
echo Building...
cmake --build . --config Release --parallel

if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

echo.
echo Build complete!
echo Executable: %BUILD_DIR%\Release\EdoProPlus.exe
cd ..\..
