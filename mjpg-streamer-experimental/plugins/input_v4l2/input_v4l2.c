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
#define JPEG_BUFFER_SIZE (2 * 1024 * 1024) // 2MB JPEG buffer
#define MAX_BUFFERS 4
#define MAX_INPUT_PLUGINS 10

// 转换类型枚举
enum {
    CONV_NONE = 0,    // 不需要转换
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
    int conversion_type;  // 转换类型
    unsigned char* jpeg_buffer; // JPEG转换缓冲区
} context;

// 静态数组存储所有插件实例的上下文
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
    
    // 存储上下文指针
    plugin_contexts[id] = ctx;
    
    // 使用param传递的参数
    int argc = param->argc;
    char **argv = param->argv;
    
    // 调试输出参数
    printf("input_init called with %d arguments:\n", argc);
    for (int i = 0; i < argc; i++) {
        printf("  argv[%d] = %s\n", i, argv[i] ? argv[i] : "(null)");
    }
    
    static struct option long_options[] = {
        {"device", required_argument, 0, 'd'},
        {"resolution", required_argument, 0, 'r'},
        {"fps", required_argument, 0, 'f'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    // 重置getopt
    optind = 0;
    
    // 解析选项
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
                printf("V4L2 input plugin options:\n");
                printf("  -d, --device <device>   V4L2 device (default: /dev/video0)\n");
                printf("  -r, --resolution <res>  Resolution (e.g., VGA, HD, 640x480)\n");
                printf("  -f, --fps <fps>         Frames per second\n");
                free(ctx);
                plugin_contexts[id] = NULL;
                return 0; // 不是错误，但需要停止初始化
            default:
                fprintf(stderr, "Unknown option\n");
                free(ctx);
                plugin_contexts[id] = NULL;
                return -1;
        }
    }

    // 设置默认值
    if (!ctx->device) ctx->device = strdup("/dev/video0");
    if (!ctx->width) ctx->width = 640;
    if (!ctx->height) ctx->height = 480;
    if (!ctx->fps) ctx->fps = 0;  // 0 表示使用驱动默认帧率

    printf("Opening device: %s\n", ctx->device);
    printf("Resolution: %dx%d\n", ctx->width, ctx->height);
    printf("FPS: %d (0 = driver default)\n", ctx->fps);

    // 打开设备
    ctx->v4l2.fd = v4l2_open(ctx->device);
    if (ctx->v4l2.fd < 0) {
        fprintf(stderr, "Error opening V4L2 device %s\n", ctx->device);
        free(ctx->device);
        free(ctx);
        plugin_contexts[id] = NULL;
        return -1;
    }
    
    printf("Device opened: fd=%d\n", ctx->v4l2.fd);
    
    // 检查设备能力
    struct v4l2_capability caps;
    CLEAR(caps);
    if (ioctl(ctx->v4l2.fd, VIDIOC_QUERYCAP, &caps) == 0) {
        printf("Driver: %s\n", caps.driver);
        printf("Card: %s\n", caps.card);
        printf("Bus: %s\n", caps.bus_info);
        printf("Capabilities: %08x\n", caps.capabilities);
    } else {
        perror("VIDIOC_QUERYCAP failed");
    }

    // STM32 dcmipp 驱动特殊处理
    int is_stm32_dcmipp = 0;
    if (strcmp((char*)caps.driver, "dcmipp") == 0) {
        printf("STM32 dcmipp driver detected, applying workaround\n");
        is_stm32_dcmipp = 1;
        
        // 此驱动不支持帧率控制
        ctx->fps = 0;
    }

    // 尝试支持的格式 - 优先 MJPEG
    int supported_formats[] = {
        V4L2_PIX_FMT_MJPEG,  // 优先尝试 MJPEG
        V4L2_PIX_FMT_JPEG,   // 然后尝试 JPEG
        V4L2_PIX_FMT_YUYV,   // 再尝试 YUYV
        V4L2_PIX_FMT_RGBP    // 最后尝试 RGBP
    };
    
    const char *format_names[] = {
        "MJPEG", "JPEG", "YUYV", "RGBP"
    };
    
    int format_count = sizeof(supported_formats) / sizeof(supported_formats[0]);
    int format_success = 0;
    
    for (int i = 0; i < format_count; i++) {
        printf("Trying format: %s...\n", format_names[i]);
        if (v4l2_init(&ctx->v4l2, ctx->width, ctx->height, ctx->fps, supported_formats[i]) == 0) {
            // 获取实际设置的格式
            struct v4l2_format actual_fmt;
            CLEAR(actual_fmt);
            actual_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            
            if (ioctl(ctx->v4l2.fd, VIDIOC_G_FMT, &actual_fmt) == 0) {
                printf("Actual format set: %c%c%c%c\n",
                    (actual_fmt.fmt.pix.pixelformat >> 0) & 0xFF,
                    (actual_fmt.fmt.pix.pixelformat >> 8) & 0xFF,
                    (actual_fmt.fmt.pix.pixelformat >> 16) & 0xFF,
                    (actual_fmt.fmt.pix.pixelformat >> 24) & 0xFF);
            }
            
            // 根据格式设置转换类型
            if (supported_formats[i] == V4L2_PIX_FMT_JPEG || 
                supported_formats[i] == V4L2_PIX_FMT_MJPEG) {
                ctx->conversion_type = CONV_NONE;
            } else if (supported_formats[i] == V4L2_PIX_FMT_YUYV) {
                ctx->conversion_type = CONV_YUYV_TO_JPEG;
            } else if (supported_formats[i] == V4L2_PIX_FMT_RGBP) {
                ctx->conversion_type = CONV_RGBP_TO_JPEG;
            }
            
            format_success = 1;
            printf("Format %s initialized successfully\n", format_names[i]);
            break;
        }
    }
    
    if (!format_success) {
        fprintf(stderr, "Failed to initialize any supported format\n");
        close(ctx->v4l2.fd);
        free(ctx->device);
        free(ctx);
        plugin_contexts[id] = NULL;
        return -1;
    }

    // 分配JPEG缓冲区（如果需要转换）
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

    // STM32 dcmipp 驱动特殊处理 - 缓冲区数量
    if (is_stm32_dcmipp) {
        // 对于STM32，我们可能需要调整缓冲区数量
        // 这已经在v4l2_init中处理
    }

    // 开始捕获
    printf("Starting capture...\n");
    if (v4l2_start_capture(&ctx->v4l2) < 0) {
        fprintf(stderr, "Start capture failed\n");
        
        // 尝试使用DMA缓冲区作为后备方案
        if (is_stm32_dcmipp) {
            printf("Trying DMA buffers as fallback...\n");
            
            // 关闭并重新打开设备
            v4l2_close(&ctx->v4l2);
            close(ctx->v4l2.fd);
            
            ctx->v4l2.fd = v4l2_open(ctx->device);
            if (ctx->v4l2.fd < 0) {
                fprintf(stderr, "Error reopening V4L2 device %s\n", ctx->device);
                if (ctx->jpeg_buffer) free(ctx->jpeg_buffer);
                free(ctx->device);
                free(ctx);
                plugin_contexts[id] = NULL;
                return -1;
            }
            
            // 重新初始化格式
            if (v4l2_init(&ctx->v4l2, ctx->width, ctx->height, ctx->fps, ctx->conversion_type) != 0) {
                fprintf(stderr, "Reinitialization failed\n");
                if (ctx->jpeg_buffer) free(ctx->jpeg_buffer);
                v4l2_close(&ctx->v4l2);
                free(ctx->device);
                free(ctx);
                plugin_contexts[id] = NULL;
                return -1;
            }
            
            // 再次尝试捕获
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
            // 非STM32设备，直接失败
            if (ctx->jpeg_buffer) free(ctx->jpeg_buffer);
            v4l2_close(&ctx->v4l2);
            free(ctx->device);
            free(ctx);
            plugin_contexts[id] = NULL;
            return -1;
        }
    }

    printf("V4L2 input plugin initialized successfully\n");
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

    // 捕获帧
    int index = v4l2_capture_frame(&ctx->v4l2);
    if (index < 0 || index >= MAX_BUFFERS) {
        fprintf(stderr, "Invalid buffer index: %d\n", index);
        return -1;
    }
    
    // 获取原始帧数据
    unsigned char* raw_frame = ctx->v4l2.buffers[index];
    size_t raw_size = ctx->v4l2.buf.bytesused;
    
    // 根据格式处理
    switch (ctx->conversion_type) {
        case CONV_NONE:
            // 直接使用JPEG/MJPEG
            ctx->frame = raw_frame;
            ctx->frame_size = raw_size;
            break;
            
        case CONV_YUYV_TO_JPEG:
            // YUYV转JPEG
            ctx->frame_size = compress_yuyv_to_jpeg(
                ctx->jpeg_buffer, JPEG_BUFFER_SIZE,
                raw_frame, 
                ctx->width, ctx->height,
                85
            );
            if (ctx->frame_size > 0 && ctx->frame_size <= JPEG_BUFFER_SIZE) {
                ctx->frame = ctx->jpeg_buffer;
            } else {
                fprintf(stderr, "YUYV to JPEG conversion failed\n");
                return -1;
            }
            break;
            
        case CONV_RGBP_TO_JPEG:
            // RGBP转JPEG
            ctx->frame_size = compress_rgbp_to_jpeg(
                ctx->jpeg_buffer, JPEG_BUFFER_SIZE,
                raw_frame, 
                ctx->width, ctx->height,
                85
            );
            if (ctx->frame_size > 0 && ctx->frame_size <= JPEG_BUFFER_SIZE) {
                ctx->frame = ctx->jpeg_buffer;
            } else {
                fprintf(stderr, "RGBP to JPEG conversion failed\n");
                return -1;
            }
            break;
            
        default:
            fprintf(stderr, "Unknown conversion type: %d\n", ctx->conversion_type);
            return -1;
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
    if (id < 0 || id >= MAX_INPUT_PLUGINS) {
        fprintf(stderr, "Invalid plugin ID: %d\n", id);
        return -1;
    }
    
    context *ctx = plugin_contexts[id];
    if (!ctx) {
        printf("No context for instance %d\n", id);
        return 0;
    }

    printf("Stopping capture...\n");
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(ctx->v4l2.fd, VIDIOC_STREAMOFF, &type) < 0) {
        perror("Stop capture failed");
    }
    v4l2_close(&ctx->v4l2);
    
    // 释放资源
    if (ctx->device) {
        free(ctx->device);
        ctx->device = NULL;
    }
    if (ctx->jpeg_buffer) {
        free(ctx->jpeg_buffer);
        ctx->jpeg_buffer = NULL;
    }
    free(ctx);
    
    // 清空指针
    plugin_contexts[id] = NULL;
    
    printf("V4L2 input plugin stopped\n");
    return 0;
}

/* 插件控制接口 */
int input_cmd(int command, unsigned int parameter, unsigned int parameter2, int parameter3, char* parameter_string) {
    // 默认使用第一个输入插件的上下文
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