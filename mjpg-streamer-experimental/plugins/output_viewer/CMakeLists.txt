
find_package(SDL2 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(TURBOJPEG REQUIRED libturbojpeg)

MJPG_STREAMER_PLUGIN_OPTION(output_viewer "SDL2 Viewer Plugin"
                            ONLYIF SDL2_FOUND TURBOJPEG_FOUND)

if (PLUGIN_OUTPUT_VIEWER)
    include_directories(
        ${SDL2_INCLUDE_DIRS}
        ${TURBOJPEG_INCLUDE_DIRS}
    )
    
    MJPG_STREAMER_PLUGIN_COMPILE(output_viewer output_viewer.c)

    target_link_libraries(output_viewer
        ${SDL2_LIBRARIES}
        ${TurboJPEG_LIBRARIES} 
    )

    # ARM优化
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        message(STATUS "Building for ARMv8-A (64-bit)")
        target_compile_options(output_viewer PRIVATE 
            -O3 
            -mcpu=${PLATFORM_ARCH}
        )
    endif()
endif()