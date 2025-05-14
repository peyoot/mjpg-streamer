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

/* 初始化SDL窗口和渲染器 */
static int init_sdl(int width, int height)
{
    pthread_mutex_lock(&ctx_mutex);
    
    if (!ctx.initialized) {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            fprintf(stderr, "SDL_Init Error: %s\n", SDL_GetError());
            pthread_mutex_unlock(&ctx_mutex);
            return -1;
        }

        ctx.window = SDL_CreateWindow("MJPG-Streamer Viewer",
            SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
            width, height, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        
        if (!ctx.window) {
            fprintf(stderr, "SDL_CreateWindow Error: %s\n", SDL_GetError());
            SDL_Quit();
            pthread_mutex_unlock(&ctx_mutex);
            return -1;
        }

        ctx.renderer = SDL_CreateRenderer(ctx.window, -1, 
            SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        
        if (!ctx.renderer) {
            SDL_DestroyWindow(ctx.window);
            SDL_Quit();
            pthread_mutex_unlock(&ctx_mutex);
            return -1;
        }

        ctx.texture = SDL_CreateTexture(ctx.renderer,
            SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
            width, height);
        
        if (!ctx.texture) {
            SDL_DestroyRenderer(ctx.renderer);
            SDL_DestroyWindow(ctx.window);
            SDL_Quit();
            pthread_mutex_unlock(&ctx_mutex);
            return -1;
        }

        ctx.width = width;
        ctx.height = height;
        ctx.initialized = 1;
    }
    
    pthread_mutex_unlock(&ctx_mutex);
    return 0;
}

/* 事件处理 */
static void process_events(void)
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            pthread_mutex_lock(&pglobal->in[input_number].db);
            pglobal->stop = 1;
            pthread_mutex_unlock(&pglobal->in[input_number].db);
        }
        else if (event.type == SDL_WINDOWEVENT) {
            if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                pthread_mutex_lock(&ctx_mutex);
                SDL_RenderSetLogicalSize(ctx.renderer, 
                    event.window.data1, event.window.data2);
                pthread_mutex_unlock(&ctx_mutex);
            }
        }
    }
}

/* 工作线程 */
static void *worker_thread(void *arg)
{
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    unsigned char *jpeg_buf = NULL;
    unsigned char *rgb_buf = NULL;
    size_t buf_size = 4096 * 1024;

    jpeg_buf = malloc(buf_size);
    if (!jpeg_buf) {
        OPRINT("Failed to allocate JPEG buffer\n");
        return NULL;
    }

    while (!pglobal->stop) {
        process_events();

        /* 获取JPEG数据 */
        pthread_mutex_lock(&pglobal->in[input_number].db);
        pthread_cond_wait(&pglobal->in[input_number].db_update,
                         &pglobal->in[input_number].db);
        
        size_t frame_size = pglobal->in[input_number].size;
        if (frame_size > buf_size) {
            unsigned char *new_buf = realloc(jpeg_buf, frame_size);
            if (!new_buf) {
                OPRINT("Failed to realloc JPEG buffer\n");
                pthread_mutex_unlock(&pglobal->in[input_number].db);
                continue;
            }
            jpeg_buf = new_buf;
            buf_size = frame_size;
        }
        memcpy(jpeg_buf, pglobal->in[input_number].buf, frame_size);
        pthread_mutex_unlock(&pglobal->in[input_number].db);

        /* JPEG解码 */
        cinfo.err = jpeg_std_error(&jerr);
        jpeg_create_decompress(&cinfo);
        jpeg_mem_src(&cinfo, jpeg_buf, frame_size);
        
        if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
            jpeg_destroy_decompress(&cinfo);
            continue;
        }

        cinfo.out_color_space = JCS_RGB;
        jpeg_start_decompress(&cinfo);
        
        int width = cinfo.output_width;
        int height = cinfo.output_height;
        rgb_buf = malloc(width * height * 3);
        
        JSAMPROW row_ptr[1];
        while (cinfo.output_scanline < height) {
            row_ptr[0] = &rgb_buf[cinfo.output_scanline * width * 3];
            jpeg_read_scanlines(&cinfo, row_ptr, 1);
        }

        /* 更新显示 */
        if (init_sdl(width, height) == 0) {
            pthread_mutex_lock(&ctx_mutex);
            SDL_UpdateTexture(ctx.texture, NULL, rgb_buf, width * 3);
            SDL_RenderClear(ctx.renderer);
            SDL_RenderCopy(ctx.renderer, ctx.texture, NULL, NULL);
            SDL_RenderPresent(ctx.renderer);
            pthread_mutex_unlock(&ctx_mutex);
        }

        free(rgb_buf);
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
    }

    free(jpeg_buf);
    return NULL;
}

/* 插件接口 */
int output_init(output_parameter *param)
{
    int opt;
    while ((opt = getopt(param->argc, param->argv, "i:")) != -1) {
        if (opt == 'i') input_number = atoi(optarg);
    }
    pglobal = param->global;
    return 0;
}

int output_run(int id)
{
    if (pthread_create(&worker, NULL, worker_thread, NULL) != 0) {
        OPRINT("Failed to create worker thread\n");
        return -1;
    }
    pthread_detach(worker);
    return 0;
}

int output_stop(int id)
{
    pthread_mutex_lock(&ctx_mutex);
    if (ctx.texture) SDL_DestroyTexture(ctx.texture);
    if (ctx.renderer) SDL_DestroyRenderer(ctx.renderer);
    if (ctx.window) SDL_DestroyWindow(ctx.window);
    SDL_Quit();
    memset(&ctx, 0, sizeof(ctx));
    pthread_mutex_unlock(&ctx_mutex);
    return 0;
}