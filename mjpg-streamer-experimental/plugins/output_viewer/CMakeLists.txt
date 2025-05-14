find_package(SDL2 REQUIRED)
find_package(JPEG REQUIRED)

MJPG_STREAMER_PLUGIN_OPTION(output_viewer "SDL output viewer plugin"
                            ONLYIF JPEG_FOUND SDL2_FOUND)

if (PLUGIN_OUTPUT_VIEWER)
    include_directories(${SDL2_INCLUDE_DIRS})
    MJPG_STREAMER_PLUGIN_COMPILE(output_viewer output_viewer.c)
    target_link_libraries(output_viewer 
        ${SDL2_LIBRARIES} 
        ${JPEG_LIBRARIES}
    )

    if (CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
        target_compile_options(output_viewer PRIVATE -O3 -mcpu=cortex-a55 -mfpu=neon)
    endif()
endif()