find_package(GTest CONFIG REQUIRED)
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

include(GoogleTest)
gtest_discover_tests(NativeShift.Core.Tests DISCOVERY_TIMEOUT 30)
gtest_discover_tests(NativeShift.Image.Tests DISCOVERY_TIMEOUT 30)
