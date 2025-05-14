/*******************************************************************************
#                                                                              #
#      MJPG-streamer allows to stream JPG frames from an input-plugin          #
#      to several output plugins                                               #
#                                                                              #
#      Copyright (C) 2008 Tom Stöveken                                         #
#                                                                              #
# This program is free software; you can redistribute it and/or modify         #
# it under the terms of the GNU General Public License as published by         #
# the Free Software Foundation; version 2 of the License.                      #
#                                                                              #
# This program is distributed in the hope that it will be useful,              #
# but WITHOUT ANY WARRANTY; without even the implied warranty of               #
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                #
# GNU General Public License for more details.                                 #
#                                                                              #
# You should have received a copy of the GNU General Public License            #
# along with this program; if not, write to the Free Software                  #
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA    #
#                                                                              #
*******************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <pthread.h>
#include <syslog.h>
#include <SDL2/SDL.h>
#include <jpeglib.h>
#include "../../utils.h"
#include "../../mjpg_streamer.h"

#define OUTPUT_PLUGIN_NAME "SDL2 Viewer Plugin"
#define DEBUG_PRINT(fmt, ...) fprintf(stderr, "VIEWER DEBUG: " fmt, ##__VA_ARGS__)

/* 线程安全的显示上下文 */
typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    int width;
    int height;
    int initialized;
} DisplayContext;

static pthread_mutex_t ctx_mutex = PTHREAD_MUTEX_INITIALIZER;
static DisplayContext ctx = {0};
static pthread_t worker;
static globals *pglobal;
static int input_number = 0;

/* 安全初始化SDL上下文 */
static int init_sdl_context(int width, int height) {
    pthread_mutex_lock(&ctx_mutex);
    
    if (!ctx.initialized) {
        DEBUG_PRINT("Initializing SDL subsystem\n");
        
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            pthread_mutex_unlock(&ctx_mutex);
            return -1;
        }

        ctx.window = SDL_CreateWindow("Camera Viewer - SDL2",
                                     SDL_WINDOWPOS_UNDEFINED,
                                     SDL_WINDOWPOS_UNDEFINED,
                                     width, height,
                                     SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        if (!ctx.window) {
            fprintf(stderr, "Window creation failed: %s\n", SDL_GetError());
            SDL_Quit();
            pthread_mutex_unlock(&ctx_mutex);
            return -1;
        }

        ctx.renderer = SDL_CreateRenderer(ctx.window, -1, SDL_RENDERER_ACCELERATED);
        if (!ctx.renderer) {
            fprintf(stderr, "Renderer creation failed: %s\n", SDL_GetError());
            SDL_DestroyWindow(ctx.window);
            SDL_Quit();
            pthread_mutex_unlock(&ctx_mutex);
            return -1;
        }

        ctx.texture = SDL_CreateTexture(ctx.renderer,
                                       SDL_PIXELFORMAT_RGB24,
                                       SDL_TEXTUREACCESS_STREAMING,
                                       width, height);
        if (!ctx.texture) {
            fprintf(stderr, "Texture creation failed: %s\n", SDL_GetError());
            SDL_DestroyRenderer(ctx.renderer);
            SDL_DestroyWindow(ctx.window);
            SDL_Quit();
            pthread_mutex_unlock(&ctx_mutex);
            return -1;
        }

        ctx.width = width;
        ctx.height = height;
        ctx.initialized = 1;
        DEBUG_PRINT("SDL context initialized (%dx%d)\n", width, height);
    }
    
    pthread_mutex_unlock(&ctx_mutex);
    return 0;
}

/* 安全清理资源 */
/* 安全清理资源 */
static void cleanup_sdl_context() {
    pthread_mutex_lock(&ctx_mutex);
    
    // 释放资源（SDL自身会处理NULL参数）
    SDL_DestroyTexture(ctx.texture);
    SDL_DestroyRenderer(ctx.renderer);
    SDL_DestroyWindow(ctx.window);
    
    // 关闭SDL子系统（安全可重复调用）
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    
    // 清空结构体（隐含置NULL和initialized=0）
    memset(&ctx, 0, sizeof(ctx));
    
    pthread_mutex_unlock(&ctx_mutex);
}

/* 事件处理逻辑 */
static void process_events(void) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            OPRINT("Received quit event\n");
            pthread_mutex_lock(&pglobal->in[input_number].db);
            pglobal->stop = 1;
            pthread_mutex_unlock(&pglobal->in[input_number].db);
        }
        else if (event.type == SDL_WINDOWEVENT) {
            if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                pthread_mutex_lock(&ctx_mutex);
                if (ctx.initialized) {
                    SDL_RenderSetLogicalSize(ctx.renderer, 
                                           event.window.data1,
                                           event.window.data2);
                }
                pthread_mutex_unlock(&ctx_mutex);
            }
        }
    }
}


/* 图像显示函数 */
static void show_image(unsigned char *rgb_data, int width, int height) {
    /* 检查尺寸变化 */
    if (ctx.initialized && (width != ctx.width || height != ctx.height)) {
        DEBUG_PRINT("Resolution changed %dx%d -> %dx%d\n", 
                   ctx.width, ctx.height, width, height);
        cleanup_sdl_context();
    }

    /* 初始化检查 */
    if (init_sdl_context(width, height) != 0) {
        return;
    }

    /* 更新纹理 */
    pthread_mutex_lock(&ctx_mutex);
    SDL_UpdateTexture(ctx.texture, NULL, rgb_data, width * 3);
    SDL_RenderClear(ctx.renderer);
    SDL_RenderCopy(ctx.renderer, ctx.texture, NULL, NULL);
    SDL_RenderPresent(ctx.renderer);
    pthread_mutex_unlock(&ctx_mutex);

    /* 处理事件 */
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            DEBUG_PRINT("Received SDL_QUIT event\n");
            pglobal->stop = 1;
        } else if (event.type == SDL_WINDOWEVENT) {
            if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                SDL_RenderSetLogicalSize(ctx.renderer, width, height);
            }
        }
    }
}

/* JPEG解码线程 */
static void *worker_thread(void *arg) {
    unsigned char *jpeg_frame = NULL;
    size_t jpeg_frame_size = 4096 * 1024; // 初始缓冲区大小
    int frame_size = 0;

    if ((jpeg_frame = malloc(jpeg_frame_size)) == NULL) {
        OPRINT("JPEG buffer allocation failed\n");
        return NULL;
    }

    while (!pglobal->stop) {       
        process_events(); // 主循环中处理事件
        /* 获取帧数据 */
        pthread_mutex_lock(&pglobal->in[input_number].db);
        pthread_cond_wait(&pglobal->in[input_number].db_update,
                         &pglobal->in[input_number].db);
        frame_size = pglobal->in[input_number].size;

        // 动态调整缓冲区大小
        if (frame_size > jpeg_frame_size) {
            unsigned char *new_buf = realloc(jpeg_frame, frame_size);
            if (!new_buf) {
                OPRINT("Failed to realloc jpeg buffer to %d bytes\n", frame_size);
                pthread_mutex_unlock(&pglobal->in[input_number].db);
                continue;
            }
            jpeg_frame = new_buf;
            jpeg_frame_size = frame_size;
        }

        memcpy(jpeg_frame, pglobal->in[input_number].buf, frame_size);
        pthread_mutex_unlock(&pglobal->in[input_number].db);

        /* JPEG解码 */
        struct jpeg_decompress_struct cinfo;
        struct jpeg_error_mgr jerr;
        unsigned char *rgb_buffer = NULL;
        int decode_ok = 1;

        cinfo.err = jpeg_std_error(&jerr);
        jpeg_create_decompress(&cinfo);
        jpeg_mem_src(&cinfo, jpeg_frame, frame_size);

        if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
            OPRINT("JPEG header invalid\n");
            jpeg_destroy_decompress(&cinfo);
            continue;
        }

        cinfo.out_color_space = JCS_RGB; // 确保输出为RGB格式
        jpeg_start_decompress(&cinfo);
        int width = cinfo.output_width;
        int height = cinfo.output_height;

        /* 分配RGB缓冲区 */
        if ((rgb_buffer = malloc(width * height * 3)) == NULL) {
            OPRINT("RGB buffer allocation failed\n");
            jpeg_abort_decompress(&cinfo);
            jpeg_destroy_decompress(&cinfo);
            continue;
        }

        /* 逐行解码 */
        while (cinfo.output_scanline < height) {
            unsigned char *row = rgb_buffer + cinfo.output_scanline * width * 3;
            JSAMPROW row_ptr[1] = {row}; // 修正行指针传递方式
            if (jpeg_read_scanlines(&cinfo, row_ptr, 1) != 1) {
                OPRINT("JPEG scanline error at line %d\n", cinfo.output_scanline);
                decode_ok = 0;
                break;
            }
        }

        if (decode_ok) {
            show_image(rgb_buffer, width, height);
        }

        free(rgb_buffer);
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
    }

    free(jpeg_frame);
    cleanup_sdl_context();
    return NULL;
}

/* 插件接口 */
int output_init(output_parameter *param) {
    OPRINT("Starting output_init with %d parameters\n", param->argc);
    for(int i=0; i<param->argc; i++){
        OPRINT("argv[%d] = %s\n", i, param->argv[i]);
    }

    // 处理帮助参数
    if (param->argc >= 1 && strcmp(param->argv[0], "--help") == 0) {
        fprintf(stderr, "Usage: %s [-i <input_number>]\n", param->argv[0]);
        return 2;
    }

    int opt;

    while ((opt = getopt(param->argc, param->argv, "i:")) != -1) {
        if (opt == -1) break; // 修复getopt循环条件

        switch (opt) {
        case 'i':
            input_number = atoi(optarg);
            OPRINT("Set input number to %d\n", input_number);
            break;
        default:
            OPRINT("Unknown option: %c\n", opt);
            return -1;
        }
    }

    pglobal = param->global;

    if (!pglobal) {
        OPRINT("Global context not available\n");
        return -1;
    }

    // 验证输入插件编号
    if (input_number >= pglobal->incnt) {
        OPRINT("Invalid input number %d (max %d)\n", input_number, pglobal->incnt-1);
        return -1;
    }

    return 0;
}

int output_run(int id) {
    // 确保只初始化一次视频子系统
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            OPRINT("SDL_Init failed: %s\n", SDL_GetError());
            return -1;
        }
        OPRINT("SDL video subsystem initialized\n");
    } else {
        OPRINT("SDL video already initialized\n");
    }

    // 创建线程前检查全局状态
    if (pglobal->stop) {
        OPRINT("Already in stop state\n");
        return -1;
    }

    if (pthread_create(&worker, NULL, worker_thread, NULL) != 0) {
        OPRINT("Worker thread creation failed\n");
        SDL_Quit();
        return -1;
    }
    pthread_detach(worker);
    return 0;
}

int output_stop(int id) {
    pglobal->stop = 1;
    cleanup_sdl_context();
    SDL_Quit();
    return 0;
}

int output_cmd(int plugin, unsigned int control, unsigned int group, int value) {
    return 0;
}