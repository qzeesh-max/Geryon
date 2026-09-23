@echo off
REM Geryon - A Distributed Shared Memory Framework
REM Copyright (C) 2026 Zeeshan Qazi
REM SPDX-License-Identifier: AGPL-3.0-or-later
REM
REM Build Geryon natively on Windows.
REM Usage: scripts\build.bat

cd /d "%~dp0\.."

if not exist build mkdir build
cd build

cmake .. -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
    echo CMake configuration failed.
    exit /b 1
)

cmake --build . --config Release -j %NUMBER_OF_PROCESSORS%
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

echo Build complete. Executables are in build\tests\ and build\benchmarks\.
