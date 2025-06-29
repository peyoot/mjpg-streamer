cmake_minimum_required(VERSION 2.8)
project(input_v4l2)

# Set the plugin name
set(INPUT_PLUGIN_NAME input_v4l2)

# Set the source files
set(SRC_FILES
    input_v4l2.c
)

# Include MJPG-Streamer headers
include_directories(
    ${CMAKE_SOURCE_DIR}/../..
    ${CMAKE_SOURCE_DIR}/../../utils
)

# Create the shared library
add_library(${INPUT_PLUGIN_NAME} SHARED ${SRC_FILES})

# Set the output name to input_v4l2.so
set_target_properties(${INPUT_PLUGIN_NAME} PROPERTIES
    PREFIX ""
    SUFFIX ".so"
)

# Link against required libraries
target_link_libraries(${INPUT_PLUGIN_NAME}
    pthread
    jpeg
)
