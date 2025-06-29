check_include_files(linux/videodev2.h HAVE_LINUX_VIDEODEV2_H)

MJPG_STREAMER_PLUGIN_OPTION(input_v4l2 "MIPI Camera via V4L2"
                            ONLYIF HAVE_LINUX_VIDEODEV2_H)

if (PLUGIN_INPUT_V4L2)
    add_definitions(-DLINUX -D_GNU_SOURCE)
    
    # 查找依赖库
    find_library(V4L2_LIB v4l2)
    find_library(JPEG_LIB jpeg)
    
    # 编译插件
    MJPG_STREAMER_PLUGIN_COMPILE(input_v4l2 
        input_v4l2.c
        v4l2_utils.c
    )
    
    # 链接库
    if(V4L2_LIB)
        target_link_libraries(input_v4l2 ${V4L2_LIB})
    endif()
    
    if(JPEG_LIB)
        target_link_libraries(input_v4l2 ${JPEG_LIB})
    else()
        message(WARNING "libjpeg not found, MJPEG support disabled")
    endif()
endif()