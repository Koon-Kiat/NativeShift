find_package(FFMPEG REQUIRED)

add_library(
    NativeShift.Media
    src/audio_transcoder.cpp
    src/media_capabilities.cpp
    src/media_provider.cpp
    src/video_encoder.cpp
    src/video_transcoder.cpp
)

add_library(NativeShift::Media ALIAS NativeShift.Media)

target_include_directories(
    NativeShift.Media
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_include_directories(
    NativeShift.Media
    SYSTEM PRIVATE
        ${FFMPEG_INCLUDE_DIRS}
)

target_link_directories(NativeShift.Media PRIVATE ${FFMPEG_LIBRARY_DIRS})

target_link_libraries(
    NativeShift.Media
    PUBLIC
        NativeShift::Core
    PRIVATE
        ${FFMPEG_LIBRARIES}
)
