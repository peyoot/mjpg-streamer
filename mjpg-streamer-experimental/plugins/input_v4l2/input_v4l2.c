#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <linux/videodev2.h>
#include "v4l2_utils.h"
#include "../../mjpg_streamer.h"
#include "../../utils.h"

// 手动定义命令常量（参考 input_uvc）
#ifndef CMD_INPUT_GET_IMAGE
#define CMD_INPUT_GET_IMAGE 0
#endif

#define INPUT_PLUGIN_NAME "V4L2 input plugin"
#define MAX_ARGUMENTS 32

// 手动声明parse函数
int parse(char *in, char **argv, int max);

typedef struct {
    v4l2_dev_t v4l2;
    int width;
    int height;
    int fps;
    char* device;
    unsigned char* frame;
    size_t frame_size;
} context;

static context ctx;

/* 插件初始化 */
int input_init(input_parameter *param, int id) {
    memset(&ctx, 0, sizeof(context));
    
    // 解析参数
    char *argv[MAX_ARGUMENTS];
    int argc = parse(param->argv[0], argv, MAX_ARGUMENTS);
    
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i+1 < argc) {
            ctx.device = strdup(argv[++i]);
        }
        else if (!strcmp(argv[i], "-r") && i+1 < argc) {
            sscanf(argv[++i], "%dx%d", &ctx.width, &ctx.height);
        }
        else if (!strcmp(argv[i], "-f") && i+1 < argc) {
            ctx.fps = atoi(argv[++i]);
        }
    }

    // 默认值
    if (!ctx.device) ctx.device = strdup("/dev/video0");
    if (!ctx.width) ctx.width = 640;
    if (!ctx.height) ctx.height = 480;
    if (!ctx.fps) ctx.fps = 30;

    // 打开设备
    ctx.v4l2.fd = v4l2_open(ctx.device);
    if (ctx.v4l2.fd < 0) {
        fprintf(stderr, "Error opening V4L2 device %s\n", ctx.device);
        return -1;
    }

    // 初始化设备
    if (v4l2_init(&ctx.v4l2, ctx.width, ctx.height, ctx.fps) < 0) {
        fprintf(stderr, "V4L2 init failed\n");
        return -1;
    }

    // 开始捕获
    if (v4l2_start_capture(&ctx.v4l2) < 0) {
        fprintf(stderr, "Start capture failed\n");
        return -1;
    }

    return 0;
}

/* 获取一帧图像 */
int input_run(int id) {
    int index = v4l2_capture_frame(&ctx.v4l2);
    if (index < 0) return -1;
    
    // 获取帧数据
    ctx.frame = ctx.v4l2.buffers[index];
    ctx.frame_size = ctx.v4l2.buf.bytesused;
    
    // 将缓冲重新加入队列
    if (ioctl(ctx.v4l2.fd, VIDIOC_QBUF, &ctx.v4l2.buf) < 0) {
        perror("Requeue buffer failed");
        return -1;
    }
    
    return 0;
}

/* 停止捕获 */
int input_stop(int id) {
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(ctx.v4l2.fd, VIDIOC_STREAMOFF, &type) < 0) {
        perror("Stop capture failed");
    }
    v4l2_close(&ctx.v4l2);
    if (ctx.device) {
        free(ctx.device);
        ctx.device = NULL;
    }
    return 0;
}

/* 插件控制接口 - 使用匹配的函数签名 */
int input_cmd(int command, unsigned int parameter, unsigned int parameter2, int parameter3, char* parameter_string) {
    switch (command) {
        case CMD_INPUT_GET_IMAGE:
            *((unsigned char**)parameter_string) = ctx.frame;
            *((int*)(uintptr_t)parameter) = ctx.frame_size;
            break;
        default:
            return -1;
    }
    return 0;
}

/* 插件描述 */
static struct _input input_plugin_v4l2 = {
    .name = INPUT_PLUGIN_NAME,
    .init = input_init,
    .run = input_run,
    .stop = input_stop,
    .cmd = input_cmd
};

/* 插件注册 */
struct _input* input_get_plugin() {
    return &input_plugin_v4l2;
}