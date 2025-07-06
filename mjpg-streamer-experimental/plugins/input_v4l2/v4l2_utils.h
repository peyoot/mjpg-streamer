#pragma once

#include <linux/videodev2.h>
#include <string.h>  // 添加 memset 声明
#include <stdint.h>  // 添加 uint32_t 定义

// 确保必要的像素格式已定义
#ifndef V4L2_PIX_FMT_YUYV
#define V4L2_PIX_FMT_YUYV v4l2_fourcc('Y', 'U', 'Y', 'V')
#endif

#ifndef V4L2_PIX_FMT_MJPEG
#define V4L2_PIX_FMT_MJPEG v4l2_fourcc('M', 'J', 'P', 'G')
#endif

#define CLEAR(x) memset(&(x), 0, sizeof(x))

typedef struct {
    int fd;
    struct v4l2_format fmt;
    struct v4l2_buffer buf;
    void* buffers[4];  // 增加缓冲区数量
    uint32_t n_buffers;
} v4l2_dev_t;

int v4l2_open(const char* device);
int v4l2_init(v4l2_dev_t* dev, int width, int height, int fps, int format);
int v4l2_start_capture(v4l2_dev_t* dev);
int v4l2_capture_frame(v4l2_dev_t* dev);
void v4l2_close(v4l2_dev_t* dev);