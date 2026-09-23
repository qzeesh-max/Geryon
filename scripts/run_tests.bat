@echo off
cd %~dp0\..
call scripts\build.bat
cd build
ctest --build-config Release --output-on-failure
