cmake_minimum_required(VERSION 3.28)

project(
    NativeShift
    VERSION 0.2.0
    DESCRIPTION "Privacy-focused native Windows file conversion engine"
    LANGUAGES CXX
)

option(NATIVESHIFT_BUILD_CLI "Build the converter command-line application" ON)
option(NATIVESHIFT_BUILD_TESTS "Build unit and integration tests" ON)
option(NATIVESHIFT_BUILD_BENCHMARKS "Build reproducible core benchmarks" ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ProjectWarnings)

add_subdirectory(src/NativeShift.Core)
add_subdirectory(src/NativeShift.Image)
add_subdirectory(src/NativeShift.Media)
add_subdirectory(src/NativeShift.Platform.Windows)
add_subdirectory(src/NativeShift.GuiBridge)

if(NATIVESHIFT_BUILD_CLI)
    add_subdirectory(src/NativeShift.Cli)
endif()

if(NATIVESHIFT_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(tests)
endif()

if(NATIVESHIFT_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
