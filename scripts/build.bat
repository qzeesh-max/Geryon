@echo off
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release -j %NUMBER_OF_PROCESSORS%
