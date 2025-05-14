find_package(SDL2 REQUIRED)
find_package(TurboJPEG REQUIRED)

MJPG_STREAMER_PLUGIN_OPTION(output_viewer "SDL2 Viewer Plugin"
                            ONLYIF TurboJPEG_FOUND SDL2_FOUND)

if (PLUGIN_OUTPUT_VIEWER)
    include_directories(
        ${SDL2_INCLUDE_DIRS}
        ${TurboJPEG_INCLUDE_DIRS}
    )
    
    MJPG_STREAMER_PLUGIN_COMPILE(output_viewer output_viewer.c)
    
    target_link_libraries(output_viewer
        ${SDL2_LIBRARIES}
        ${TurboJPEG_LIBRARIES}  
    )
    
    # ARM平台优化
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
        if (CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
            target_compile_options(output_viewer PRIVATE -O3 -mcpu=cortex-a55)
        else()
            target_compile_options(output_viewer PRIVATE 
                -O3 
                -mcpu=cortex-a35 
                -mfpu=neon-fp-armv8 
                -mfloat-abi=hard
            )
        endif()
    endif()
endif()