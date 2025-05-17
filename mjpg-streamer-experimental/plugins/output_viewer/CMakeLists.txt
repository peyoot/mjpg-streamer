# plugins/output_viewer/CMakeLists.txt

# 查找必要依赖
find_package(PkgConfig REQUIRED)

# 查找GStreamer核心库
pkg_check_modules(GSTREAMER REQUIRED
    gstreamer-1.0 >= 1.16
    gstreamer-app-1.0
    gstreamer-video-1.0
)

# 查找Wayland协议
pkg_check_modules(WAYLAND_CLIENT REQUIRED
    wayland-client >= 1.18
    wayland-protocols >= 1.21
)

# 查找V4L2相关库
pkg_check_modules(LIBV4L2 REQUIRED libv4l2)

# 配置插件选项
MJPG_STREAMER_PLUGIN_OPTION(output_viewer "GStreamer/Wayland Viewer Plugin"
    ONLYIF GSTREAMER_FOUND WAYLAND_CLIENT_FOUND LIBV4L2_FOUND)

if (PLUGIN_OUTPUT_VIEWER)
    # 包含目录
    include_directories(
        ${GSTREAMER_INCLUDE_DIRS}
        ${WAYLAND_CLIENT_INCLUDE_DIRS}
        ${LIBV4L2_INCLUDE_DIRS}
    )

    # 添加编译定义
    add_definitions(
        -D_GNU_SOURCE
        -DHAVE_GST_WAYLAND=1
    )

    # 编译插件
    MJPG_STREAMER_PLUGIN_COMPILE(output_viewer output_viewer.c)

    # 链接库
    target_link_libraries(output_viewer
        ${GSTREAMER_LIBRARIES}
        ${WAYLAND_CLIENT_LIBRARIES}
        ${LIBV4L2_LIBRARIES}
        pthread
        rt
    )

    # Yocto平台特殊优化
    if(CMAKE_SYSTEM_NAME MATCHES "Linux")
        # 嵌入式平台优化选项
        target_compile_options(output_viewer PRIVATE
            -O3
            -mcpu=${PLATFORM_ARCH}
            -fstack-protector-strong
            -Wformat
            -Werror=format-security
        )

        # 链接器优化
        target_link_options(output_viewer PRIVATE
            -Wl,-O1
            -Wl,--hash-style=gnu
            -Wl,--as-needed
        )
    endif()

    # 显示配置摘要
    message(STATUS "GStreamer includes: ${GSTREAMER_INCLUDE_DIRS}")
    message(STATUS "Wayland includes: ${WAYLAND_CLIENT_INCLUDE_DIRS}")
    message(STATUS "Linked libraries: ${GSTREAMER_LIBRARIES} ${WAYLAND_CLIENT_LIBRARIES}")
endif()