
/******************************************************************************
MJPG-streamer input plugin for V4L2 devices.
******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#include "../../mjpg_streamer.h"
#include "../../utils.h"


extern void callback_new_frame(unsigned char *buffer, size_t len);

static int fd = -1;
static pthread_t capture_thread;
static int width = 640;
static int height = 480;
static int fps = 30;
static int use_yuyv = 0;
static char device[128] = "/dev/video0";

struct buffer {
    void *start;
    size_t length;
};
static struct buffer *buffers;
static unsigned int n_buffers;

static void *capture_loop(void *arg) {
    while (1) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(fd, VIDIOC_DQBUF, &buf) == -1) {
            perror("VIDIOC_DQBUF");
            continue;
        }

        if (callback_new_frame) {
            callback_new_frame((unsigned char *)buffers[buf.index].start, buf.bytesused);
        }

        if (ioctl(fd, VIDIOC_QBUF, &buf) == -1) {
            perror("VIDIOC_QBUF");
        }
    }
    return NULL;
}

int input_init(input_parameter *param, int id) {
    char *arg = strtok(param->parameters, " ");
    while (arg) {
        if (strcmp(arg, "-d") == 0) {
            arg = strtok(NULL, " ");
            if (arg) strncpy(device, arg, sizeof(device) - 1);
        } else if (strcmp(arg, "-r") == 0) {
            arg = strtok(NULL, " ");
            if (arg) sscanf(arg, "%dx%d", &width, &height);
        } else if (strcmp(arg, "-f") == 0) {
            arg = strtok(NULL, " ");
            if (arg) fps = atoi(arg);
        } else if (strcmp(arg, "-y") == 0) {
            use_yuyv = 1;
        }
        arg = strtok(NULL, " ");
    }

    fd = open(device, O_RDWR);
    if (fd == -1) {
        perror("Opening video device");
        return 1;
    }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = use_yuyv ? V4L2_PIX_FMT_YUYV : V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    if (ioctl(fd, VIDIOC_S_FMT, &fmt) == -1) {
        perror("Setting Pixel Format");
        return 1;
    }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd, VIDIOC_REQBUFS, &req) == -1) {
        perror("Requesting Buffer");
        return 1;
    }

    buffers = calloc(req.count, sizeof(*buffers));
    for (n_buffers = 0; n_buffers < req.count; ++n_buffers) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = n_buffers;

        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) == -1) {
            perror("Querying Buffer");
            return 1;
        }

        buffers[n_buffers].length = buf.length;
        buffers[n_buffers].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        if (buffers[n_buffers].start == MAP_FAILED) {
            perror("mmap");
            return 1;
        }
    }

    for (unsigned int i = 0; i < n_buffers; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(fd, VIDIOC_QBUF, &buf) == -1) {
            perror("VIDIOC_QBUF");
            return 1;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) == -1) {
        perror("VIDIOC_STREAMON");
        return 1;
    }

    if (pthread_create(&capture_thread, NULL, capture_loop, NULL)) {
        perror("Creating capture thread");
        return 1;
    }

    return 0;
}

int input_stop() {
    if (fd != -1) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(fd, VIDIOC_STREAMOFF, &type);
        close(fd);
        fd = -1;
    }
    return 0;
}

void input_help() {
    fprintf(stderr, "V4L2 input plugin:\n"
                    "  -d <device>       : video device (default: /dev/video0)\n"
                    "  -r <width>x<height>: resolution (default: 640x480)\n"
                    "  -f <fps>          : frame rate (default: 30)\n"
                    "  -y                : use YUYV format instead of MJPEG\n");
}
