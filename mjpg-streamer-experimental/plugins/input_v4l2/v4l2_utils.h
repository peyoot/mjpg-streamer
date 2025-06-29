#pragma once

#include <linux/videodev2.h>
#include <stdint.h>

#define CLEAR(x) memset(&(x), 0, sizeof(x))

typedef struct {
    int fd;
    struct v4l2_format fmt;
    struct v4l2_buffer buf;
    void* buffers[2];
    uint32_t n_buffers;
} v4l2_dev_t;

int v4l2_open(const char* device);
int v4l2_init(v4l2_dev_t* dev, int width, int height, int fps);
int v4l2_start_capture(v4l2_dev_t* dev);
int v4l2_capture_frame(v4l2_dev_t* dev);
void v4l2_close(v4l2_dev_t* dev);