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
    
    // 验证格式
    if (dev->fmt.fmt.pix.pixelformat != format) {
        fprintf(stderr, "Driver set format %c%c%c%c instead of %c%c%c%c\n",
                (dev->fmt.fmt.pix.pixelformat) & 0xFF,
                (dev->fmt.fmt.pix.pixelformat >> 8) & 0xFF,
                (dev->fmt.fmt.pix.pixelformat >> 16) & 0xFF,
                (dev->fmt.fmt.pix.pixelformat >> 24) & 0xFF,
                format & 0xFF,
                (format >> 8) & 0xFF,
                (format >> 16) & 0xFF,
                (format >> 24) & 0xFF);
        return -1;
    }

    // 设置帧率
    struct v4l2_streamparm parm;
    CLEAR(parm);
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = fps;
    
    if (ioctl(dev->fd, VIDIOC_S_PARM, &parm) < 0) {
        perror("Setting FPS failed");
    }

    // 请求缓冲区
    struct v4l2_requestbuffers req;
    CLEAR(req);
    req.count = MAX_BUFFERS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(dev->fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("Requesting buffers failed");
        return -1;
    }

    // 内存映射
    dev->n_buffers = req.count;
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