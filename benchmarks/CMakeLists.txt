add_executable(
    NativeShift.Benchmarks
    benchmark_main.cpp
)

target_link_libraries(
    NativeShift.Benchmarks
    PRIVATE
        NativeShift::Core
        NativeShift::Image
        NativeShift::Media
        NativeShift::Platform.Windows
        nativeshift_project_options
)
