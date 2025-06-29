#include "v4l2_utils.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>

int v4l2_open(const char* device) {
    int fd = open(device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", device, strerror(errno));
    }
    return fd;
}

int v4l2_init(v4l2_dev_t* dev, int width, int height, int fps) {
    // 设置格式 (MIPI摄像头通常支持YUV/MJPEG)
    CLEAR(dev->fmt);
    dev->fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    dev->fmt.fmt.pix.width = width;
    dev->fmt.fmt.pix.height = height;
    dev->fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG; // 或V4L2_PIX_FMT_YUYV
    dev->fmt.fmt.pix.field = V4L2_FIELD_ANY;
    
    if (ioctl(dev->fd, VIDIOC_S_FMT, &dev->fmt) < 0) {
        perror("Setting format failed");
        return -1;
    }

    // 设置帧率
    struct v4l2_streamparm parm;
    CLEAR(parm);
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = fps;
    ioctl(dev->fd, VIDIOC_S_PARM, &parm);

    // 请求缓冲区
    struct v4l2_requestbuffers req;
    CLEAR(req);
    req.count = 2;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(dev->fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("Requesting buffers failed");
        return -1;
    }

    // 内存映射
    for (dev->n_buffers = 0; dev->n_buffers < req.count; ++dev->n_buffers) {
        CLEAR(dev->buf);
        dev->buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        dev->buf.memory = V4L2_MEMORY_MMAP;
        dev->buf.index = dev->n_buffers;

        if (ioctl(dev->fd, VIDIOC_QUERYBUF, &dev->buf) < 0) {
            perror("Querying buffer failed");
            return -1;
        }

        dev->buffers[dev->n_buffers] = mmap(
            NULL, dev->buf.length,
            PROT_READ | PROT_WRITE, MAP_SHARED,
            dev->fd, dev->buf.m.offset
        );

        if (dev->buffers[dev->n_buffers] == MAP_FAILED) {
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
        perror("Start capture failed");
        return -1;
    }
    return 0;
}

int v4l2_capture_frame(v4l2_dev_t* dev) {
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
        close(dev->fd);
        dev->fd = -1;
    }
}