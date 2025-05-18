# plugins/output_viewer/CMakeLists.txt
find_package(PkgConfig REQUIRED)
find_package(TurboJPEG REQUIRED)

# 修正后的GStreamer检查（注意引号）
pkg_check_modules(GSTREAMER REQUIRED
    "gstreamer-1.0 >= 1.16"
    gstreamer-app-1.0
    gstreamer-video-1.0
)

pkg_check_modules(WAYLAND_CLIENT REQUIRED
    wayland-client
    "wayland-protocols >= 1.21"
)

pkg_check_modules(LIBV4L2 REQUIRED libv4l2)

MJPG_STREAMER_PLUGIN_OPTION(output_viewer "GStreamer/Wayland Viewer Plugin"
    ONLYIF GSTREAMER_FOUND WAYLAND_CLIENT_FOUND LIBV4L2_FOUND)

if (PLUGIN_OUTPUT_VIEWER)
    include_directories(
        ${GSTREAMER_INCLUDE_DIRS}
        ${WAYLAND_CLIENT_INCLUDE_DIRS}
        ${LIBV4L2_INCLUDE_DIRS}
    )

    add_definitions(
        -D_GNU_SOURCE
        -DHAVE_GST_WAYLAND=1
    )

    MJPG_STREAMER_PLUGIN_COMPILE(output_viewer output_viewer.c)

    target_link_libraries(output_viewer
        ${GSTREAMER_LIBRARIES}
        ${WAYLAND_CLIENT_LIBRARIES}
        ${LIBV4L2_LIBRARIES}
        ${TurboJPEG_LIBRARIES}
        pthread
        rt
    )

    if(CMAKE_SYSTEM_NAME MATCHES "Linux")
        target_compile_options(output_viewer PRIVATE
            -O3
            -mcpu=${PLATFORM_ARCH}
            -fstack-protector-strong
            -Wformat
            -Werror=format-security
        )

        target_link_options(output_viewer PRIVATE
            -Wl,-O1
            -Wl,--hash-style=gnu
            -Wl,--as-needed
        )
    endif()
endif()