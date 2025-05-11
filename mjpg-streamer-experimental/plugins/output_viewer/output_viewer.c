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
static void cleanup_sdl_context() {
    pthread_mutex_lock(&ctx_mutex);
    
    if (ctx.texture) {
        SDL_DestroyTexture(ctx.texture);
        ctx.texture = NULL;
    }
    if (ctx.renderer) {
        SDL_DestroyRenderer(ctx.renderer);
        ctx.renderer = NULL;
    }
    if (ctx.window) {
        SDL_DestroyWindow(ctx.window);
        ctx.window = NULL;
    }
    SDL_Quit();
    ctx.initialized = 0;
    
    DEBUG_PRINT("SDL resources cleaned up\n");
    pthread_mutex_unlock(&ctx_mutex);
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
    int frame_size = 0;

    if ((jpeg_frame = malloc(4096 * 1024)) == NULL) {
        OPRINT("JPEG buffer allocation failed\n");
        return NULL;
    }

    while (!pglobal->stop) {
        /* 获取帧数据 */
        pthread_mutex_lock(&pglobal->in[input_number].db);
        pthread_cond_wait(&pglobal->in[input_number].db_update,
                         &pglobal->in[input_number].db);
        frame_size = pglobal->in[input_number].size;
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
            if (jpeg_read_scanlines(&cinfo, &row, 1) != 1) {
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
    int opt;
    while ((opt = getopt(param->argc, param->argv, "i:")) != -1) {
        if (opt == 'i') input_number = atoi(optarg);
    }
    pglobal = param->global;
    return 0;
}

int output_run(int id) {
    if (pthread_create(&worker, NULL, worker_thread, NULL) != 0) {
        OPRINT("Worker thread creation failed\n");
        return -1;
    }
    pthread_detach(worker);
    return 0;
}

int output_stop(int id) {
    pglobal->stop = 1;
    cleanup_sdl_context();
    return 0;
}

int output_cmd(int plugin, unsigned int control, unsigned int group, int value) {
    return 0;
}
