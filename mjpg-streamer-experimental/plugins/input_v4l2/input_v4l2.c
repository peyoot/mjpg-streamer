#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <getopt.h>
#include <linux/videodev2.h>
#include "v4l2_utils.h"
#include "jpeg_utils.h"
#include "../../mjpg_streamer.h"
#include "../../utils.h"

#ifndef INPUT_GET_IMAGE
#define INPUT_GET_IMAGE 0
#endif

#define INPUT_PLUGIN_NAME "V4L2 input plugin"
#define MAX_ARGUMENTS 32
#define JPEG_BUFFER_SIZE (2 * 1024 * 1024) // 2MB JPEG buffer
#define MAX_BUFFERS 4

typedef struct {
    v4l2_dev_t v4l2;
    int width;
    int height;
    int fps;
    char* device;
    unsigned char* frame;
    size_t frame_size;
    int need_conversion;
    unsigned char* jpeg_buffer;
    globals *global;  // 存储全局指针
    int instance_id;  // 存储实例ID
} context;

/* 插件初始化 */
int input_init(input_parameter *param, int id) {
    context *ctx = calloc(1, sizeof(context));
    if (!ctx) {
        fprintf(stderr, "Memory allocation failed\n");
        return -1;
    }
    
    // 保存全局指针和实例ID
    ctx->global = param->global;
    ctx->instance_id = id;
    
    // 将上下文存储到全局结构中
    param->global->in[id].context = ctx;
    
    int argc = 0;
    char *argv[MAX_ARGUMENTS];
    char *token, *saveptr;
    
    char *input = strdup(param->argv[0]);
    if (!input) {
        fprintf(stderr, "Memory allocation failed\n");
        free(ctx);
        return -1;
    }
    
    token = strtok_r(input, " ", &saveptr);
    while (token != NULL && argc < MAX_ARGUMENTS) {
        argv[argc++] = token;
        token = strtok_r(NULL, " ", &saveptr);
    }
    
    static struct option long_options[] = {
        {"device", required_argument, 0, 'd'},
        {"resolution", required_argument, 0, 'r'},
        {"fps", required_argument, 0, 'f'},
        {0, 0, 0, 0}
    };
    
    reset_getopt();
    
    int c;
    while ((c = getopt_long(argc, argv, "d:r:f:", long_options, NULL)) != -1) {
        switch (c) {
            case 'd':
                ctx->device = strdup(optarg);
                break;
            case 'r':
                parse_resolution_opt(optarg, &ctx->width, &ctx->height);
                break;
            case 'f':
                ctx->fps = atoi(optarg);
                break;
            default:
                fprintf(stderr, "Unknown option: %c\n", c);
                free(input);
                free(ctx);
                return -1;
        }
    }
    
    free(input);

    // 设置默认值
    if (!ctx->device) ctx->device = strdup("/dev/video0");
    if (!ctx->width) ctx->width = 640;
    if (!ctx->height) ctx->height = 480;
    if (!ctx->fps) ctx->fps = 30;

    printf("Opening device: %s, Resolution: %dx%d, FPS: %d\n", 
           ctx->device, ctx->width, ctx->height, ctx->fps);

    // 打开设备
    ctx->v4l2.fd = v4l2_open(ctx->device);
    if (ctx->v4l2.fd < 0) {
        fprintf(stderr, "Error opening V4L2 device %s\n", ctx->device);
        free(ctx->device);
        free(ctx);
        return -1;
    }

    // 尝试MJPEG格式
    int format = V4L2_PIX_FMT_MJPEG;
    if (v4l2_init(&ctx->v4l2, ctx->width, ctx->height, ctx->fps, format) < 0) {
        fprintf(stderr, "MJPEG format not supported, trying YUYV\n");
        format = V4L2_PIX_FMT_YUYV;
        if (v4l2_init(&ctx->v4l2, ctx->width, ctx->height, ctx->fps, format) < 0) {
            fprintf(stderr, "V4L2 init failed\n");
            close(ctx->v4l2.fd);
            free(ctx->device);
            free(ctx);
            return -1;
        }
        ctx->need_conversion = 1;
    } else {
        ctx->need_conversion = 0;
    }
    
    // 分配JPEG缓冲区
    if (ctx->need_conversion) {
        ctx->jpeg_buffer = malloc(JPEG_BUFFER_SIZE);
        if (!ctx->jpeg_buffer) {
            fprintf(stderr, "Failed to allocate JPEG buffer\n");
            v4l2_close(&ctx->v4l2);
            free(ctx->device);
            free(ctx);
            return -1;
        }
    }

    // 开始捕获
    if (v4l2_start_capture(&ctx->v4l2) < 0) {
        fprintf(stderr, "Start capture failed\n");
        if (ctx->jpeg_buffer) free(ctx->jpeg_buffer);
        v4l2_close(&ctx->v4l2);
        free(ctx->device);
        free(ctx);
        return -1;
    }

    return 0;
}

/* 获取一帧图像 */
int input_run(int id) {
    // 直接从全局结构中获取上下文
    context *ctx = (context *)global.in[id].context;
    if (!ctx) {
        fprintf(stderr, "Context is NULL for instance %d\n", id);
        return -1;
    }

    int index = v4l2_capture_frame(&ctx->v4l2);
    if (index < 0 || index >= MAX_BUFFERS) {
        fprintf(stderr, "Invalid buffer index: %d\n", index);
        return -1;
    }
    
    // 获取原始帧数据
    unsigned char* raw_frame = ctx->v4l2.buffers[index];
    size_t raw_size = ctx->v4l2.buf.bytesused;
    
    // 根据格式处理
    if (ctx->need_conversion) {
        // YUYV转JPEG
        int jpeg_size = compress_yuyv_to_jpeg(
            ctx->jpeg_buffer, JPEG_BUFFER_SIZE,
            raw_frame, 
            ctx->width, ctx->height,
            85
        );
        
        if (jpeg_size > 0 && jpeg_size <= JPEG_BUFFER_SIZE) {
            ctx->frame = ctx->jpeg_buffer;
            ctx->frame_size = jpeg_size;
        } else {
            fprintf(stderr, "YUV to JPEG conversion failed or buffer overflow: %d\n", jpeg_size);
            return -1;
        }
    } else {
        // 直接使用MJPEG
        ctx->frame = raw_frame;
        ctx->frame_size = raw_size;
    }
    
    // 重新入队缓冲区
    if (ioctl(ctx->v4l2.fd, VIDIOC_QBUF, &ctx->v4l2.buf) < 0) {
        perror("Requeue buffer failed");
        return -1;
    }
    
    return 0;
}

/* 停止捕获 */
int input_stop(int id) {
    // 直接从全局结构中获取上下文
    context *ctx = (context *)global.in[id].context;
    if (!ctx) return 0;

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(ctx->v4l2.fd, VIDIOC_STREAMOFF, &type) < 0) {
        perror("Stop capture failed");
    }
    v4l2_close(&ctx->v4l2);
    
    // 释放资源
    if (ctx->device) free(ctx->device);
    if (ctx->jpeg_buffer) free(ctx->jpeg_buffer);
    free(ctx);
    
    // 清空全局指针
    global.in[id].context = NULL;
    
    return 0;
}

/* 插件控制接口 */
int input_cmd(int command, unsigned int parameter, unsigned int parameter2, int parameter3, char* parameter_string) {
    // 默认使用第一个输入插件的上下文
    context *ctx = (context *)global.in[0].context;
    
    if (!ctx || !ctx->frame) {
        return -1;
    }
    
    switch (command) {
        case INPUT_GET_IMAGE:
            if (parameter_string) {
                *((unsigned char**)parameter_string) = ctx->frame;
            }
            if (parameter) {
                *((int*)(uintptr_t)parameter) = ctx->frame_size;
            }
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