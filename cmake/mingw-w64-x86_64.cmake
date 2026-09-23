set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Specify the cross-compiler
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)

# Where is the target environment
set(CMAKE_FIND_ROOT_PATH /usr/local/x86_64-w64-mingw32 /opt/homebrew/x86_64-w64-mingw32)

# Adjust the default behaviour of the FIND_XXX() commands:
# Search for headers and libraries in the target environment, search
# programs in the host environment
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Linker flags to resolve Windows networking and multithreading
set(CMAKE_EXE_LINKER_FLAGS "-static -lws2_32 -lwsock32 -lmswsock" CACHE STRING "LDFLAGS")
