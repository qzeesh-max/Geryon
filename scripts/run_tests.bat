@echo off
REM Geryon - A Distributed Shared Memory Framework
REM Copyright (C) 2026 Zeeshan Qazi
REM SPDX-License-Identifier: AGPL-3.0-or-later
REM
REM Build and run the Geryon test suite on Windows.
REM Usage: scripts\run_tests.bat [extra-ctest-args...]

cd /d "%~dp0\.."

call scripts\build.bat
if errorlevel 1 exit /b 1

cd build
ctest --build-config Release --output-on-failure %*
