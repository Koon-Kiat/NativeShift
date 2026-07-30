find_package(GTest CONFIG REQUIRED)
find_package(FFMPEG REQUIRED)
find_package(PNG REQUIRED)
find_package(WebP CONFIG REQUIRED)

add_executable(
    NativeShift.Core.Tests
    NativeShift.Core.Tests/core_tests.cpp
)
target_link_libraries(
    NativeShift.Core.Tests
    PRIVATE
        NativeShift::Core
        GTest::gtest_main
        nativeshift_project_options
)

add_executable(
    NativeShift.Image.Tests
    NativeShift.Image.Tests/image_tests.cpp
)
target_link_libraries(
    NativeShift.Image.Tests
    PRIVATE
        NativeShift::Core
        NativeShift::Image
        GTest::gtest_main
        PNG::PNG
        WebP::webpdecoder
        nativeshift_project_options
)

add_executable(
    NativeShift.Media.Tests
    NativeShift.Media.Tests/media_tests.cpp
)

add_executable(
    NativeShift.GuiBridge.Tests
    NativeShift.GuiBridge.Tests/gui_bridge_tests.cpp
)
target_link_libraries(
    NativeShift.GuiBridge.Tests
    PRIVATE
        NativeShift::GuiBridge
        GTest::gtest_main
        nativeshift_project_options
)
add_custom_command(
    TARGET NativeShift.GuiBridge.Tests
    POST_BUILD
    COMMAND
        ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:NativeShiftGuiBridge>"
        "$<TARGET_FILE_DIR:NativeShift.GuiBridge.Tests>"
    COMMAND_EXPAND_LISTS
)
target_include_directories(
    NativeShift.Media.Tests
    SYSTEM PRIVATE
        ${FFMPEG_INCLUDE_DIRS}
)
target_link_directories(NativeShift.Media.Tests PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(
    NativeShift.Media.Tests
    PRIVATE
        NativeShift::Core
        NativeShift::Media
        GTest::gtest_main
        ${FFMPEG_LIBRARIES}
        nativeshift_project_options
)

include(GoogleTest)
gtest_discover_tests(NativeShift.Core.Tests DISCOVERY_TIMEOUT 30)
gtest_discover_tests(NativeShift.Image.Tests DISCOVERY_TIMEOUT 30)
gtest_discover_tests(NativeShift.Media.Tests DISCOVERY_TIMEOUT 30)
gtest_discover_tests(NativeShift.GuiBridge.Tests DISCOVERY_TIMEOUT 30)
