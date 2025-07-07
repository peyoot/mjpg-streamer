// input_v4l2.c
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
#define JPEG_BUFFER_SIZE (2 * 1024 * 1024)
#define MAX_BUFFERS 4
#define MAX_INPUT_PLUGINS 10

enum {
    CONV_NONE = 0,
    CONV_YUYV_TO_JPEG,
    CONV_RGBP_TO_JPEG
};

typedef struct {
    v4l2_dev_t v4l2;
    int width;
    int height;
    int fps;
    char* device;
    unsigned char* frame;
    size_t frame_size;
    int conversion_type;
    unsigned char* jpeg_buffer;
} context;

static context *plugin_contexts[MAX_INPUT_PLUGINS] = {NULL};

/* 插件初始化 */
int input_init(input_parameter *param, int id) {
    if (id < 0 || id >= MAX_INPUT_PLUGINS) {
        fprintf(stderr, "Invalid plugin ID: %d\n", id);
        return -1;
    }
    
    context *ctx = calloc(1, sizeof(context));
    if (!ctx) {
        fprintf(stderr, "Memory allocation failed\n");
        return -1;
    }
    
    plugin_contexts[id] = ctx;
    
    int argc = param->argc;
    char **argv = param->argv;
    
    static struct option long_options[] = {
        {"device", required_argument, 0, 'd'},
        {"resolution", required_argument, 0, 'r'},
        {"fps", required_argument, 0, 'f'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    optind = 0;
    
    int c;
    while ((c = getopt_long(argc, argv, "d:r:f:h", long_options, NULL)) != -1) {
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
            case 'h':
                free(ctx);
                plugin_contexts[id] = NULL;
                return 0;
            default:
                free(ctx);
                plugin_contexts[id] = NULL;
                return -1;
        }
    }

    if (!ctx->device) ctx->device = strdup("/dev/video0");
    if (!ctx->width) ctx->width = 640;
    if (!ctx->height) ctx->height = 480;
    if (!ctx->fps) ctx->fps = 0;

    ctx->v4l2.fd = v4l2_open(ctx->device);
    if (ctx->v4l2.fd < 0) {
        fprintf(stderr, "Error opening V4L2 device %s\n", ctx->device);
        free(ctx->device);
        free(ctx);
        plugin_contexts[id] = NULL;
        return -1;
    }
    
    struct v4l2_capability caps;
    CLEAR(caps);
    if (ioctl(ctx->v4l2.fd, VIDIOC_QUERYCAP, &caps) != 0) {
        perror("VIDIOC_QUERYCAP failed");
    }

    int is_stm32_dcmipp = 0;
    if (strcmp((char*)caps.driver, "dcmipp") == 0) {
        is_stm32_dcmipp = 1;
        ctx->fps = 0;
    }

    int supported_formats[] = {
        V4L2_PIX_FMT_MJPEG,
        V4L2_PIX_FMT_JPEG,
        V4L2_PIX_FMT_YUYV,
        V4L2_PIX_FMT_RGBP
    };
    
    int format_count = sizeof(supported_formats) / sizeof(supported_formats[0]);
    int format_success = 0;
    
    for (int i = 0; i < format_count; i++) {
        if (ctx->v4l2.fd >= 0) {
            close(ctx->v4l2.fd);
            ctx->v4l2.fd = -1;
        }
        
        ctx->v4l2.fd = v4l2_open(ctx->device);
        if (ctx->v4l2.fd < 0) {
            continue;
        }
        
        if (v4l2_init(&ctx->v4l2, ctx->width, ctx->height, ctx->fps, supported_formats[i]) == 0) {
            struct v4l2_format actual_fmt;
            CLEAR(actual_fmt);
            actual_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            
            if (ioctl(ctx->v4l2.fd, VIDIOC_G_FMT, &actual_fmt) == 0) {
                uint32_t pix_fmt = actual_fmt.fmt.pix.pixelformat;
                if (pix_fmt != supported_formats[i]) {
                    close(ctx->v4l2.fd);
                    ctx->v4l2.fd = -1;
                    continue;
                }
            } else {
                close(ctx->v4l2.fd);
                ctx->v4l2.fd = -1;
                continue;
            }
            
            if (supported_formats[i] == V4L2_PIX_FMT_JPEG || 
                supported_formats[i] == V4L2_PIX_FMT_MJPEG) {
                ctx->conversion_type = CONV_NONE;
            } else if (supported_formats[i] == V4L2_PIX_FMT_YUYV) {
                ctx->conversion_type = CONV_YUYV_TO_JPEG;
            } else if (supported_formats[i] == V4L2_PIX_FMT_RGBP) {
                ctx->conversion_type = CONV_RGBP_TO_JPEG;
            }
            
            format_success = 1;
            break;
        } else {
            close(ctx->v4l2.fd);
            ctx->v4l2.fd = -1;
        }
    }
    
    if (!format_success) {
        fprintf(stderr, "Failed to initialize any supported format\n");
        if (ctx->v4l2.fd >= 0) close(ctx->v4l2.fd);
        free(ctx->device);
        free(ctx);
        plugin_contexts[id] = NULL;
        return -1;
    }

    if (ctx->conversion_type != CONV_NONE) {
        ctx->jpeg_buffer = malloc(JPEG_BUFFER_SIZE);
        if (!ctx->jpeg_buffer) {
            fprintf(stderr, "Failed to allocate JPEG buffer\n");
            v4l2_close(&ctx->v4l2);
            free(ctx->device);
            free(ctx);
            plugin_contexts[id] = NULL;
            return -1;
        }
    }

    if (v4l2_start_capture(&ctx->v4l2) < 0) {
        fprintf(stderr, "Start capture failed\n");
        
        if (is_stm32_dcmipp) {
            v4l2_close(&ctx->v4l2);
            
            ctx->v4l2.fd = v4l2_open(ctx->device);
            if (ctx->v4l2.fd < 0) {
                fprintf(stderr, "Error reopening V4L2 device %s\n", ctx->device);
                if (ctx->jpeg_buffer) free(ctx->jpeg_buffer);
                free(ctx->device);
                free(ctx);
                plugin_contexts[id] = NULL;
                return -1;
            }
            
            if (v4l2_init(&ctx->v4l2, ctx->width, ctx->height, ctx->fps, ctx->conversion_type) != 0) {
                fprintf(stderr, "Reinitialization failed\n");
                if (ctx->jpeg_buffer) free(ctx->jpeg_buffer);
                v4l2_close(&ctx->v4l2);
                free(ctx->device);
                free(ctx);
                plugin_contexts[id] = NULL;
                return -1;
            }
            
            if (v4l2_start_capture(&ctx->v4l2) < 0) {
                fprintf(stderr, "Fallback capture start failed\n");
                if (ctx->jpeg_buffer) free(ctx->jpeg_buffer);
                v4l2_close(&ctx->v4l2);
                free(ctx->device);
                free(ctx);
                plugin_contexts[id] = NULL;
                return -1;
            }
        } else {
            if (ctx->jpeg_buffer) free(ctx->jpeg_buffer);
            v4l2_close(&ctx->v4l2);
            free(ctx->device);
            free(ctx);
            plugin_contexts[id] = NULL;
            return -1;
        }
    }

    return 0;
}

/* 获取一帧图像 */
int input_run(int id) {
    if (id < 0 || id >= MAX_INPUT_PLUGINS) {
        fprintf(stderr, "Invalid plugin ID: %d\n", id);
        return -1;
    }
    
    context *ctx = plugin_contexts[id];
    if (!ctx) {
        fprintf(stderr, "Context is NULL for instance %d\n", id);
        return -1;
    }

    int index = v4l2_capture_frame(&ctx->v4l2);
    if (index < 0 || index >= MAX_BUFFERS) {
        return -1;
    }
    
    unsigned char* raw_frame = ctx->v4l2.buffers[index];
    size_t raw_size = ctx->v4l2.buf.bytesused;
    
    switch (ctx->conversion_type) {
        case CONV_NONE:
            ctx->frame = raw_frame;
            ctx->frame_size = raw_size;
            break;
            
        case CONV_YUYV_TO_JPEG:
            ctx->frame_size = compress_yuyv_to_jpeg(
                ctx->jpeg_buffer, JPEG_BUFFER_SIZE,
                raw_frame, 
                ctx->width, ctx->height,
                85
            );
            if (ctx->frame_size > 0) {
                ctx->frame = ctx->jpeg_buffer;
            } else {
                return -1;
            }
            break;
            
        case CONV_RGBP_TO_JPEG:
            ctx->frame_size = compress_rgbp_to_jpeg(
                ctx->jpeg_buffer, JPEG_BUFFER_SIZE,
                raw_frame, 
                ctx->width, ctx->height,
                85
            );
            if (ctx->frame_size > 0) {
                ctx->frame = ctx->jpeg_buffer;
            } else {
                return -1;
            }
            break;
            
        default:
            return -1;
    }
    
    if (ioctl(ctx->v4l2.fd, VIDIOC_QBUF, &ctx->v4l2.buf) < 0) {
        return -1;
    }
    
    return 0;
}

/* 停止捕获 */
int input_stop(int id) {
    if (id < 0 || id >= MAX_INPUT_PLUGINS) {
        return -1;
    }
    
    context *ctx = plugin_contexts[id];
    if (!ctx) {
        return 0;
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(ctx->v4l2.fd, VIDIOC_STREAMOFF, &type);
    v4l2_close(&ctx->v4l2);
    
    if (ctx->device) {
        free(ctx->device);
    }
    if (ctx->jpeg_buffer) {
        free(ctx->jpeg_buffer);
    }
    free(ctx);
    
    plugin_contexts[id] = NULL;
    
    return 0;
}

/* 插件控制接口 */
int input_cmd(int command, unsigned int parameter, unsigned int parameter2, int parameter3, char* parameter_string) {
    if (plugin_contexts[0]) {
        context *ctx = plugin_contexts[0];
        
        if (!ctx->frame) {
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
    return -1;
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