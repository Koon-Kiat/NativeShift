add_executable(
    NativeShift.Cli
    app.manifest
    src/main.cpp
)

set_target_properties(
    NativeShift.Cli
    PROPERTIES
        OUTPUT_NAME "nativeshift-cli"
)

target_link_libraries(
    NativeShift.Cli
    PRIVATE
        NativeShift::Core
        NativeShift::Image
        NativeShift::Platform.Windows
        nativeshift_project_options
)

if(MSVC)
    target_link_options(NativeShift.Cli PRIVATE /SUBSYSTEM:CONSOLE)
endif()

install(TARGETS NativeShift.Cli RUNTIME DESTINATION bin)
