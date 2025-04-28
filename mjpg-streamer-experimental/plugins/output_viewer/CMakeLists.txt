find_package(SDL2 REQUIRED)

MJPG_STREAMER_PLUGIN_OPTION(output_viewer "SDL2 output viewer plugin" 
                            ONLYIF JPEG_LIB SDL2_FOUND)

if (PLUGIN_OUTPUT_VIEWER)
    include_directories(${SDL2_INCLUDE_DIRS})
    MJPG_STREAMER_PLUGIN_COMPILE(output_viewer output_viewer.c)
    target_link_libraries(output_viewer ${SDL2_LIBRARIES} ${JPEG_LIB})
endif()
