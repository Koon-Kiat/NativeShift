add_library(
    NativeShiftGuiBridge
    SHARED
    src/gui_bridge.cpp
)

set_target_properties(
    NativeShiftGuiBridge
    PROPERTIES
        OUTPUT_NAME "NativeShift.GuiBridge"
        WINDOWS_EXPORT_ALL_SYMBOLS OFF
)

target_include_directories(
    NativeShiftGuiBridge
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_compile_definitions(
    NativeShiftGuiBridge
    PRIVATE
        NATIVESHIFT_GUI_BRIDGE_EXPORTS
)

target_link_libraries(
    NativeShiftGuiBridge
    PRIVATE
        NativeShift::Core
        NativeShift::Image
        NativeShift::Media
        NativeShift::Platform.Windows
        nativeshift_project_options
)

install(
    TARGETS NativeShiftGuiBridge
    RUNTIME DESTINATION bin
    ARCHIVE DESTINATION lib
)

add_library(NativeShift::GuiBridge ALIAS NativeShiftGuiBridge)
