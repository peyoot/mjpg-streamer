find_package(SDL2 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(TURBOJPEG REQUIRED libturbojpeg)

MJPG_STREAMER_PLUGIN_OPTION(output_viewer "SDL2 Viewer Plugin"
                            ONLYIF SDL2_FOUND TURBOJPEG_FOUND)

# 初始化架构优化标志
set(ARCH_OPTIMIZATION_FLAGS "-O3")

if (DEFINED PLATFORM_ARCH)
    # Yocto平台显式指定的架构
    message(STATUS "Yocto Platform Architecture: ${PLATFORM_ARCH}")
    set(ARCH_OPTIMIZATION_FLAGS "${ARCH_OPTIMIZATION_FLAGS} -mcpu=${PLATFORM_ARCH}")
else()
    # 非Yocto环境的智能推断
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        # ARMv8-A默认使用cortex-a53
        set(ARCH_OPTIMIZATION_FLAGS "${ARCH_OPTIMIZATION_FLAGS} -mcpu=cortex-a53")
        message(STATUS "Defaulting to ARMv8-A cortex-a53")
    elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "armv7")
        # ARMv7默认使用cortex-a7
        set(ARCH_OPTIMIZATION_FLAGS "${ARCH_OPTIMIZATION_FLAGS} -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard")
        message(STATUS "Defaulting to ARMv7 cortex-a7")
    else()
        message(WARNING "Unknown architecture, using generic optimization")
        set(ARCH_OPTIMIZATION_FLAGS "${ARCH_OPTIMIZATION_FLAGS}")
    endif()
endif()

if (PLUGIN_OUTPUT_VIEWER)
    include_directories(
        ${SDL2_INCLUDE_DIRS}
        ${TURBOJPEG_INCLUDE_DIRS}
    )
    
    MJPG_STREAMER_PLUGIN_COMPILE(output_viewer output_viewer.c)

    # 修正目标链接（确保使用pkgconfig找到的库）
    target_link_libraries(output_viewer
        ${SDL2_LIBRARIES}
        ${TURBOJPEG_LIBRARIES}
    )

    # 应用统一的优化标志
    target_compile_options(output_viewer PRIVATE 
        ${ARCH_OPTIMIZATION_FLAGS}
    )
    
    # 调试输出编译标志
    message(STATUS "Final compilation flags: ${ARCH_OPTIMIZATION_FLAGS}")
endif()