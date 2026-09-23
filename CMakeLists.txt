cmake_minimum_required(VERSION 3.28)
project(Geryon VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Platform specific settings
if(APPLE)
    # Using Homebrew GCC-16 if specified
    if(EXISTS "/opt/homebrew/bin/g++-16")
        set(CMAKE_CXX_COMPILER "/opt/homebrew/bin/g++-16")
    endif()
endif()

# Find Boost
find_package(Boost REQUIRED)

# Add Include Directories
include_directories(include)

# Collect source files
file(GLOB_RECURSE SOURCES "src/*.cpp")

# Define the Geryon Static Library
add_library(geryon STATIC
    src/memory_region.cpp
    src/fault_handler.cpp
    src/network_node.cpp
)
target_include_directories(geryon PUBLIC 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/external/Memnon/include>
    $<INSTALL_INTERFACE:include>
)
if(TARGET Boost::headers)
    target_link_libraries(geryon PUBLIC Boost::headers)
endif()

if(WIN32)
    # Windows specific libraries (if any needed for networking/faults)
    target_link_libraries(geryon PUBLIC ws2_32 mswsock)
elseif(UNIX)
    target_link_libraries(geryon PUBLIC pthread)
endif()

# Enable Testing
enable_testing()

# Google Test via FetchContent
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.14.0
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Google Benchmark via FetchContent
FetchContent_Declare(
  benchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG        v1.8.3
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(benchmark)

# Add benchmarks
add_subdirectory(benchmarks)

# Add tests
add_subdirectory(tests)
