cmake_minimum_required(VERSION 2.8)
project(input_v4l2)

# Find required packages
find_package(Threads REQUIRED)
find_package(JPEG REQUIRED)

# Include MJPG-Streamer core headers
include_directories(
    ${CMAKE_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}/../utils
)

# Define the plugin
add_library(input_v4l2 SHARED
    input_v4l2.c
)

# Link libraries
target_link_libraries(input_v4l2
    ${CMAKE_THREAD_LIBS_INIT}
    ${JPEG_LIBRARIES}
)
