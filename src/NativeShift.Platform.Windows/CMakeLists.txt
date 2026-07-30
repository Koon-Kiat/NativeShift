add_library(
    NativeShift.Platform.Windows
    src/windows_platform_services.cpp
)

add_library(
    NativeShift::Platform.Windows
    ALIAS NativeShift.Platform.Windows
)

target_include_directories(
    NativeShift.Platform.Windows
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(
    NativeShift.Platform.Windows
    PUBLIC
        NativeShift::Core
    PRIVATE
        nativeshift_project_options
        ole32
        shell32
)
