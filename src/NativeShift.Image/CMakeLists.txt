find_package(JPEG REQUIRED)
find_package(PNG REQUIRED)
find_package(WebP CONFIG REQUIRED)

add_library(
    NativeShift.Image
    src/image_provider.cpp
)

add_library(NativeShift::Image ALIAS NativeShift.Image)

target_include_directories(
    NativeShift.Image
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(
    NativeShift.Image
    PUBLIC
        NativeShift::Core
    PRIVATE
        JPEG::JPEG
        PNG::PNG
        WebP::webp
        WebP::webpdecoder
)

if(WIN32)
    target_link_libraries(
        NativeShift.Image
        PRIVATE
            kernel32
            ole32
            oleaut32
            windowscodecs
    )
endif()
