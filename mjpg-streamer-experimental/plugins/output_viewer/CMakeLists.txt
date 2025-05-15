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
        ${TURBOJPEG_LIBRARIES}
    )

    if (DEFINED PLATFORM_ARCH)
        target_compile_options(output_viewer PRIVATE 
            -O3 
            -mcpu=${PLATFORM_ARCH}
        )
    endif()
endif()