#pragma once

#include <linux/videodev2.h>
#include <string.h>
#include <stdint.h>

#ifndef V4L2_PIX_FMT_YUYV
#define V4L2_PIX_FMT_YUYV v4l2_fourcc('Y', 'U', 'Y', 'V')
#endif

#ifndef V4L2_PIX_FMT_MJPEG
#define V4L2_PIX_FMT_MJPEG v4l2_fourcc('M', 'J', 'P', 'G')
#endif

#define CLEAR(x) memset(&(x), 0, sizeof(x))
#define MAX_BUFFERS 4

typedef struct {
    int fd;
    struct v4l2_format fmt;
    struct v4l2_buffer buf;
    void* buffers[MAX_BUFFERS];
    size_t buffer_lengths[MAX_BUFFERS]; // 存储每个缓冲区的长度
    uint32_t n_buffers;
} v4l2_dev_t;

int v4l2_open(const char* device);
int v4l2_init(v4l2_dev_t* dev, int width, int height, int fps, int format);
int v4l2_start_capture(v4l2_dev_t* dev);
int v4l2_capture_frame(v4l2_dev_t* dev);
void v4l2_close(v4l2_dev_t* dev);