
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <pthread.h>

#include "../../mjpg_streamer.h"
#include "../../utils.h"

static int width = 640;
static int height = 480;
static int fps = 30;
static int force_yuyv = 0;

static int fd = -1;
static void *buffer_start = NULL;
static size_t buffer_length = 0;
static pthread_t capture_thread;
static int stop_capturing = 0;

static int xioctl(int fd, int request, void *arg) {
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

static void *capture_loop(void *arg) {
    while (!stop_capturing) {
        fd_set fds;
        struct timeval tv;
        int r;

        FD_ZERO(&fds);
        FD_SET(fd, &fds);

        tv.tv_sec = 2;
        tv.tv_usec = 0;

        r = select(fd + 1, &fds, NULL, NULL, &tv);
        if (r == -1) {
            perror("select");
            break;
        }

        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (xioctl(fd, VIDIOC_DQBUF, &buf) == -1) {
            perror("VIDIOC_DQBUF");
            break;
        }

        callback_new_frame(buffer_start + buf.m.offset, buf.bytesused);

        if (xioctl(fd, VIDIOC_QBUF, &buf) == -1) {
            perror("VIDIOC_QBUF");
            break;
        }
    }
    return NULL;
}

int input_init(input_parameter *param, int id) {
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    struct v4l2_buffer buf;
    struct v4l2_streamparm streamparm;

    fd = open(device, O_RDWR);
    if (fd == -1) {
        perror("Opening video device");
        return 1;
    }

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = force_yuyv ? V4L2_PIX_FMT_YUYV : V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    if (xioctl(fd, VIDIOC_S_FMT, &fmt) == -1) {
        perror("Setting Pixel Format");
        return 1;
    }

    memset(&streamparm, 0, sizeof(streamparm));
    streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    streamparm.parm.capture.timeperframe.numerator = 1;
    streamparm.parm.capture.timeperframe.denominator = fps;

    xioctl(fd, VIDIOC_S_PARM, &streamparm);

    memset(&req, 0, sizeof(req));
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(fd, VIDIOC_REQBUFS, &req) == -1) {
        perror("Requesting Buffer");
        return 1;
    }

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;

    if (xioctl(fd, VIDIOC_QUERYBUF, &buf) == -1) {
        perror("Querying Buffer");
        return 1;
    }

    buffer_length = buf.length;
    buffer_start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);

    if (buffer_start == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    if (xioctl(fd, VIDIOC_QBUF, &buf) == -1) {
        perror("Queue Buffer");
        return 1;
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) == -1) {
        perror("Start Capture");
        return 1;
    }

    stop_capturing = 0;
    pthread_create(&capture_thread, NULL, capture_loop, NULL);

    return 0;
}

int input_stop() {
    stop_capturing = 1;
    pthread_join(capture_thread, NULL);
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(fd, VIDIOC_STREAMOFF, &type);
    munmap(buffer_start, buffer_length);
    close(fd);
    return 0;
}

int input_cmd(int plugin, unsigned int control, int value) {
    return 0;
}

void input_help() {
    fprintf(stderr, " -d | --device <device>     : video device (default: /dev/video0)\n");
    fprintf(stderr, " -r | --resolution <WxH>    : resolution (default: 640x480)\n");
    fprintf(stderr, " -f | --fps <fps>           : frames per second (default: 30)\n");
    fprintf(stderr, " -y                         : force YUYV format\n");
}

int input_parse_input(char *line) {
    char *arg = strtok(line, " ");
    while (arg) {
        if (strcmp(arg, "-d") == 0 || strcmp(arg, "--device") == 0) {
            arg = strtok(NULL, " ");
            if (arg) device = strdup(arg);
        } else if (strcmp(arg, "-r") == 0 || strcmp(arg, "--resolution") == 0) {
            arg = strtok(NULL, " ");
            if (arg) sscanf(arg, "%dx%d", &width, &height);
        } else if (strcmp(arg, "-f") == 0 || strcmp(arg, "--fps") == 0) {
            arg = strtok(NULL, " ");
            if (arg) fps = atoi(arg);
        } else if (strcmp(arg, "-y") == 0) {
            force_yuyv = 1;
        }
        arg = strtok(NULL, " ");
    }
    return 0;
}
