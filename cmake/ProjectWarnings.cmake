add_library(nativeshift_project_options INTERFACE)

target_compile_features(nativeshift_project_options INTERFACE cxx_std_23)

if(MSVC)
    target_compile_options(
        nativeshift_project_options
        INTERFACE
            /W4
            /WX
            /permissive-
            /EHsc
            /utf-8
            /Zc:__cplusplus
            /sdl
    )
    target_compile_definitions(
        nativeshift_project_options
        INTERFACE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
            UNICODE
            _UNICODE
    )
else()
    target_compile_options(
        nativeshift_project_options
        INTERFACE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Werror
    )
endif()
