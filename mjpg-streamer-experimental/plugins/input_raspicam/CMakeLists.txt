# 使用动态检测替代硬编码路径
find_package(PkgConfig)

# 检测关键头文件路径
find_path(VC_INCLUDE_DIR NAMES interface/vmcs_host/vcgencmd.h
          PATHS ${CMAKE_SYSROOT}/opt/vc/include
          NO_DEFAULT_PATH
          DOC "Raspberry Pi VideoCore include directory")

# 检测关键库路径
find_library(MMAL_CORE_LIB NAMES mmal_core
             PATHS ${CMAKE_SYSROOT}/opt/vc/lib
             NO_DEFAULT_PATH)
find_library(BCM_HOST_LIB NAMES bcm_host
             PATHS ${CMAKE_SYSROOT}/opt/vc/lib
             NO_DEFAULT_PATH)

# 设置综合检测标志
if(VC_INCLUDE_DIR AND MMAL_CORE_LIB AND BCM_HOST_LIB)
    set(HAS_RASPI ON)
else()
    set(HAS_RASPI OFF)
    message(WARNING "Raspberry Pi libraries not found. Disabling input_raspicam plugin.")
endif()

MJPG_STREAMER_PLUGIN_OPTION(input_raspicam "Raspberry Pi input camera plugin"
                            ONLYIF HAS_RASPI)

if (PLUGIN_INPUT_RASPICAM)
    # 添加动态发现的包含路径
    include_directories(
        ${VC_INCLUDE_DIR}
        ${VC_INCLUDE_DIR}/interface/vcos
        ${VC_INCLUDE_DIR}/interface/vcos/pthreads
        ${VC_INCLUDE_DIR}/interface/vmcs_host
        ${VC_INCLUDE_DIR}/interface/vmcs_host/linux
    )

    # 添加必要的编译定义
    add_definitions(-D_GNU_SOURCE)

    MJPG_STREAMER_PLUGIN_COMPILE(input_raspicam input_raspicam.c)

    # 链接动态发现的库
    target_link_libraries(input_raspicam
        ${MMAL_CORE_LIB}
        mmal_util
        mmal_vc_client
        vcos
        ${BCM_HOST_LIB}
    )

    # 添加系统库依赖
    target_link_libraries(input_raspicam pthread rt)

    # 安全编译选项（参考output_viewer）
    if(CMAKE_SYSTEM_NAME MATCHES "Linux")
        target_compile_options(input_raspicam PRIVATE
            -O3
            -fstack-protector-strong
            -Wformat
            -Werror=format-security
        )

        target_link_options(input_raspicam PRIVATE
            -Wl,-O1
            -Wl,--hash-style=gnu
            -Wl,--as-needed
        )
    endif()
endif()