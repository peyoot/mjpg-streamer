#pragma once

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <linux/types.h>          /* for videodev2.h */
#include <linux/videodev2.h>

// 使用相对路径包含 mjpg_streamer.h
#include "../../mjpg_streamer.h"

#define V4L2_PIX_FMT_YUYV v4l2_fourcc('Y', 'U', 'Y', 'V')
#define V4L2_PIX_FMT_MJPEG v4l2_fourcc('M', 'J', 'P', 'G')

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