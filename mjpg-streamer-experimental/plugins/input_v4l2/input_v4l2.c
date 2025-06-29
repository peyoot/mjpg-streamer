
/******************************************************************************
 * input_v4l2.c - MJPG-streamer input plugin using V4L2 interface
 * Optimized for low-latency streaming and compatible with MJPG-Streamer plugin API
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#include "../../mjpg_streamer.h"
#include "../../utils.h"

#define CLEAR(x) memset(&(x), 0, sizeof(x))

static globals *pglobal = NULL;
static int g_running = 0;

typedef struct {
    void *start;
    size_t length;
} buffer_t;

static buffer_t *buffers = NULL;
static unsigned int n_buffers = 0;
static int fd = -1;
static pthread_t capture_thread;

static char *dev_name = "/dev/video0";
static int width = 640;
static int height = 480;
static int fps = 30;
static int use_yuyv = 0;

static void errno_exit(const char *s) {
    fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
    exit(EXIT_FAILURE);
}

static int xioctl(int fh, int request, void *arg) {
    int r;
    do {
        r = ioctl(fh, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

static void *capture_loop(void *arg) {
    struct v4l2_buffer buf;

    while (g_running) {
        CLEAR(buf);
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (xioctl(fd, VIDIOC_DQBUF, &buf) == -1) {
            if (errno == EAGAIN) continue;
            perror("VIDIOC_DQBUF");
            break;
        }

        if (pglobal && pglobal->callback) {
            pglobal->callback((unsigned char *)buffers[buf.index].start, buf.bytesused);
        }

        if (xioctl(fd, VIDIOC_QBUF, &buf) == -1) {
            perror("VIDIOC_QBUF");
            break;
        }
    }

    return NULL;
}

static void init_device() {
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;

    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == -1) {
        errno_exit("VIDIOC_QUERYCAP");
    }

    CLEAR(fmt);
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = use_yuyv ? V4L2_PIX_FMT_YUYV : V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    if (xioctl(fd, VIDIOC_S_FMT, &fmt) == -1) {
        errno_exit("VIDIOC_S_FMT");
    }

    CLEAR(req);
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(fd, VIDIOC_REQBUFS, &req) == -1) {
        errno_exit("VIDIOC_REQBUFS");
    }

    buffers = calloc(req.count, sizeof(buffer_t));
    for (n_buffers = 0; n_buffers < req.count; ++n_buffers) {
        struct v4l2_buffer buf;
        CLEAR(buf);
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = n_buffers;

        if (xioctl(fd, VIDIOC_QUERYBUF, &buf) == -1) {
            errno_exit("VIDIOC_QUERYBUF");
        }

        buffers[n_buffers].length = buf.length;
        buffers[n_buffers].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        if (buffers[n_buffers].start == MAP_FAILED) {
            errno_exit("mmap");
        }
    }

    for (unsigned int i = 0; i < n_buffers; ++i) {
        struct v4l2_buffer buf;
        CLEAR(buf);
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(fd, VIDIOC_QBUF, &buf) == -1) {
            errno_exit("VIDIOC_QBUF");
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) == -1) {
        errno_exit("VIDIOC_STREAMON");
    }
}

int input_init(input_parameter *param, int id) {
    pglobal = param->global;
    g_running = 1;

    // Parse parameters
    char *arg = strtok(param->parameter_string, " ");
    while (arg) {
        if (strcmp(arg, "-d") == 0) {
            arg = strtok(NULL, " ");
            if (arg) dev_name = strdup(arg);
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

    fd = open(dev_name, O_RDWR | O_NONBLOCK, 0);
    if (fd == -1) {
        perror("Opening video device");
        return 1;
    }

    init_device();

    if (pthread_create(&capture_thread, NULL, capture_loop, NULL)) {
        perror("pthread_create");
        return 1;
    }

    return 0;
}

int input_stop() {
    g_running = 0;
    pthread_join(capture_thread, NULL);

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(fd, VIDIOC_STREAMOFF, &type);

    for (unsigned int i = 0; i < n_buffers; ++i) {
        munmap(buffers[i].start, buffers[i].length);
    }
    free(buffers);
    close(fd);
    return 0;
}

void input_help() {
    fprintf(stderr, "Options for input_v4l2:\n"
                    "  -d <device>     : video device (default: /dev/video0)\n"
                    "  -r <WxH>        : resolution (default: 640x480)\n"
                    "  -f <fps>        : frame rate (default: 30)\n"
                    "  -y              : use YUYV format instead of MJPEG\n");
}


