find_package(nlohmann_json CONFIG REQUIRED)

add_library(
    NativeShift.Core
    src/conversion_engine.cpp
    src/format_detector.cpp
    src/formats.cpp
    src/job_queue.cpp
    src/logger.cpp
    src/output_paths.cpp
    src/provider_registry.cpp
    src/settings.cpp
    src/validation.cpp
)

add_library(NativeShift::Core ALIAS NativeShift.Core)

target_include_directories(
    NativeShift.Core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(
    NativeShift.Core
    PUBLIC
        nativeshift_project_options
        nlohmann_json::nlohmann_json
)

target_compile_definitions(
    NativeShift.Core
    PUBLIC
        NATIVESHIFT_VERSION="${PROJECT_VERSION}"
)

if(WIN32)
    target_link_libraries(NativeShift.Core PRIVATE bcrypt)
endif()
