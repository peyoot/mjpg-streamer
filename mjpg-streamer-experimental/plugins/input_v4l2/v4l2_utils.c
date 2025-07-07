// v4l2_utils.c
#include "v4l2_utils.h"
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string.h>
#include <sys/select.h>
#include <stdint.h>

int v4l2_open(const char* device) {
    return open(device, O_RDWR | O_NONBLOCK);
}

int v4l2_init(v4l2_dev_t* dev, int width, int height, int fps, int format) {
    CLEAR(dev->fmt);
    dev->fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    dev->fmt.fmt.pix.width = width;
    dev->fmt.fmt.pix.height = height;
    dev->fmt.fmt.pix.pixelformat = format;
    dev->fmt.fmt.pix.field = V4L2_FIELD_ANY;
    
    if (ioctl(dev->fd, VIDIOC_S_FMT, &dev->fmt) < 0) {
        return -1;
    }
    
    struct v4l2_format actual_fmt;
    CLEAR(actual_fmt);
    actual_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    
    if (ioctl(dev->fd, VIDIOC_G_FMT, &actual_fmt) != 0) {
        return -1;
    }

    if (fps > 0) {
        struct v4l2_streamparm parm;
        CLEAR(parm);
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = fps;
        ioctl(dev->fd, VIDIOC_S_PARM, &parm);
    }

    struct v4l2_requestbuffers req;
    CLEAR(req);
    req.memory = V4L2_MEMORY_MMAP;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.count = 4;
    
    if (ioctl(dev->fd, VIDIOC_REQBUFS, &req) < 0) {
        return -1;
    }

    dev->n_buffers = req.count;
    if (dev->n_buffers > MAX_BUFFERS) {
        dev->n_buffers = MAX_BUFFERS;
    }
    
    for (uint32_t i = 0; i < dev->n_buffers; ++i) {
        CLEAR(dev->buf);
        dev->buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        dev->buf.memory = V4L2_MEMORY_MMAP;
        dev->buf.index = i;

        if (ioctl(dev->fd, VIDIOC_QUERYBUF, &dev->buf) < 0) {
            return -1;
        }

        dev->buffer_lengths[i] = dev->buf.length;
        dev->buffers[i] = mmap(NULL, dev->buf.length,
                              PROT_READ | PROT_WRITE, MAP_SHARED,
                              dev->fd, dev->buf.m.offset);

        if (dev->buffers[i] == MAP_FAILED) {
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
            return -1;
        }
    }
    
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(dev->fd, VIDIOC_STREAMON, &type) < 0) {
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
    
    if (r <= 0) {
        return -1;
    }
    
    CLEAR(dev->buf);
    dev->buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    dev->buf.memory = V4L2_MEMORY_MMAP;
    
    if (ioctl(dev->fd, VIDIOC_DQBUF, &dev->buf) < 0) {
        return -1;
    }
    
    return dev->buf.index;
}

void v4l2_close(v4l2_dev_t* dev) {
    if (dev->fd != -1) {
        for (uint32_t i = 0; i < dev->n_buffers; ++i) {
            if (dev->buffers[i]) {
                munmap(dev->buffers[i], dev->buffer_lengths[i]);
            }
        }
        close(dev->fd);
        dev->fd = -1;
    }
}