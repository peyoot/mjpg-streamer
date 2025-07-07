// v4l2_utils.c
#include "v4l2_utils.h"
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string.h>
#include <stdio.h>
#include <sys/select.h>
#include <stdint.h>
#include <sys/sysmacros.h>

int v4l2_open(const char* device) {
    int fd = open(device, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", device, strerror(errno));
    }
    return fd;
}

int v4l2_init(v4l2_dev_t* dev, int width, int height, int fps, int format) {
    // 设置格式
    CLEAR(dev->fmt);
    dev->fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    
    dev->fmt.fmt.pix.width = width;
    dev->fmt.fmt.pix.height = height;
    dev->fmt.fmt.pix.pixelformat = format;
    dev->fmt.fmt.pix.field = V4L2_FIELD_ANY;
    
    if (ioctl(dev->fd, VIDIOC_S_FMT, &dev->fmt) < 0) {
        perror("Setting format failed");
        return -1;
    }
    
    // 获取实际设置的格式
    struct v4l2_format actual_fmt;
    CLEAR(actual_fmt);
    actual_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    
    if (ioctl(dev->fd, VIDIOC_G_FMT, &actual_fmt) == 0) {
        printf("Actual format set: %c%c%c%c\n",
            (actual_fmt.fmt.pix.pixelformat >> 0) & 0xFF,
            (actual_fmt.fmt.pix.pixelformat >> 8) & 0xFF,
            (actual_fmt.fmt.pix.pixelformat >> 16) & 0xFF,
            (actual_fmt.fmt.pix.pixelformat >> 24) & 0xFF);
    } else {
        perror("WARNING: Failed to get actual format");
    }

    // 仅当帧率大于0且驱动支持时设置帧率
    if (fps > 0) {
        struct v4l2_streamparm parm;
        CLEAR(parm);
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = fps;
        
        if (ioctl(dev->fd, VIDIOC_S_PARM, &parm) < 0) {
            // 不是致命错误，记录并继续
            printf("WARNING: Setting FPS not supported, using driver default\n");
        }
    }

    // 请求缓冲区
    struct v4l2_requestbuffers req;
    CLEAR(req);
    
    // STM32 dcmipp 驱动需要 DMA 缓冲区
    req.memory = V4L2_MEMORY_MMAP;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    
    // 增加缓冲区数量以解决STM32问题
    req.count = 4;
    
    if (ioctl(dev->fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("Requesting buffers failed");
        return -1;
    }

    // 内存映射
    dev->n_buffers = req.count;
    if (dev->n_buffers > MAX_BUFFERS) {
        fprintf(stderr, "Warning: driver requested %d buffers, but only %d supported\n",
                dev->n_buffers, MAX_BUFFERS);
        dev->n_buffers = MAX_BUFFERS;
    }
    
    for (uint32_t i = 0; i < dev->n_buffers; ++i) {
        CLEAR(dev->buf);
        dev->buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        dev->buf.memory = V4L2_MEMORY_MMAP;
        dev->buf.index = i;

        if (ioctl(dev->fd, VIDIOC_QUERYBUF, &dev->buf) < 0) {
            perror("Querying buffer failed");
            return -1;
        }

        dev->buffer_lengths[i] = dev->buf.length; // 保存长度
        
        dev->buffers[i] = mmap(
            NULL, dev->buf.length,
            PROT_READ | PROT_WRITE, MAP_SHARED,
            dev->fd, dev->buf.m.offset
        );

        if (dev->buffers[i] == MAP_FAILED) {
            perror("mmap failed");
            return -1;
        }
        
        printf("Buffer %d mapped at %p, length=%u\n", 
               i, dev->buffers[i], (unsigned int)dev->buf.length);
    }
    return 0;
}

int v4l2_start_capture(v4l2_dev_t* dev) {
    for (uint32_t i = 0; i < dev->n_buffers; ++i) {
        CLEAR(dev->buf);
        dev->buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        dev->buf.memory = V4L2_MEMORY_MMAP;
        dev->buf.index = i;
        if (ioctl(dev->fd, VIDIOC_QBUF, &dev->buf) < 0) {
            perror("Queueing buffer failed");
            return -1;
        }
    }
    
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(dev->fd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON failed");
        return -1;
    }
    return 0;
}

int v4l2_capture_frame(v4l2_dev_t* dev) {
    fd_set fds;
    struct timeval tv;
    int r;
    
    FD_ZERO(&fds);
    FD_SET(dev->fd, &fds);
    
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    
    r = select(dev->fd + 1, &fds, NULL, NULL, &tv);
    
    if (r == -1) {
        perror("select error");
        return -1;
    }
    
    if (r == 0) {
        fprintf(stderr, "Capture timeout\n");
        return -1;
    }
    
    CLEAR(dev->buf);
    dev->buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    dev->buf.memory = V4L2_MEMORY_MMAP;
    
    if (ioctl(dev->fd, VIDIOC_DQBUF, &dev->buf) < 0) {
        perror("Dequeue buffer failed");
        return -1;
    }
    
    printf("Frame captured: index=%d, size=%u\n", 
           dev->buf.index, (unsigned int)dev->buf.bytesused);
    
    return dev->buf.index;
}

void v4l2_close(v4l2_dev_t* dev) {
    if (dev->fd != -1) {
        // 取消映射缓冲区（使用保存的长度）
        for (uint32_t i = 0; i < dev->n_buffers; ++i) {
            if (dev->buffers[i]) {
                munmap(dev->buffers[i], dev->buffer_lengths[i]);
                dev->buffers[i] = NULL;
            }
        }
        
        close(dev->fd);
        dev->fd = -1;
    }
}