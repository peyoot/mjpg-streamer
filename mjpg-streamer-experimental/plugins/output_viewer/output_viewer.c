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
#include <turbojpeg.h> 
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

__thread tjhandle tjInstance = NULL;

static pthread_mutex_t ctx_mutex = PTHREAD_MUTEX_INITIALIZER;
static DisplayContext ctx = {0};
static pthread_t worker;
static globals *pglobal;
static int input_number = 0;

// 预分配内存池
#define MAX_FRAME_SIZE (1920*1080*3)
static unsigned char *jpeg_buf = NULL;
static unsigned char *rgb_buf = NULL;

/* 初始化SDL窗口和渲染器 */
static int init_sdl(int width, int height)
{
    pthread_mutex_lock(&ctx_mutex);
    
    if (!ctx.initialized) {
        // 强制指定嵌入式平台渲染后端
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");

        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
            fprintf(stderr, "SDL_Init Error: %s\n", SDL_GetError());
            pthread_mutex_unlock(&ctx_mutex);
            return -1;
        }
        ctx.window = SDL_CreateWindow("MJPG-Streamer Viewer",
            SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
            width, height, 
            SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_FULLSCREEN_DESKTOP);
        
        if (!ctx.window) {
            fprintf(stderr, "SDL_CreateWindow Error: %s\n", SDL_GetError());
            SDL_Quit();
            pthread_mutex_unlock(&ctx_mutex);
            return -1;
        }
        
        // 使用硬件加速渲染器
        ctx.renderer = SDL_CreateRenderer(ctx.window, -1, 
            SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        
        if (!ctx.renderer) {
            SDL_DestroyWindow(ctx.window);
            SDL_Quit();
            pthread_mutex_unlock(&ctx_mutex);
            return -1;
        }

        // 根据屏幕实际尺寸调整
        SDL_DisplayMode dm;
        SDL_GetCurrentDisplayMode(0, &dm);
        ctx.texture = SDL_CreateTexture(ctx.renderer,
            SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
            dm.w, dm.h);
        
        if (!ctx.texture) {
            SDL_DestroyRenderer(ctx.renderer);
            SDL_DestroyWindow(ctx.window);
            SDL_Quit();
            pthread_mutex_unlock(&ctx_mutex);
            return -1;
        }

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
        if (event.type == SDL_QUIT || 
           (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_q)) {
            pthread_mutex_lock(&pglobal->in[input_number].db);
            pglobal->stop = 1;
            pthread_mutex_unlock(&pglobal->in[input_number].db);
        }
    }
}

/* 工作线程 */
static void *worker_thread(void *arg)
{
    // 预分配内存
    jpeg_buf = malloc(MAX_FRAME_SIZE);
    rgb_buf = malloc(MAX_FRAME_SIZE);
    
    if (!jpeg_buf || !rgb_buf) {
        OPRINT("Memory allocation failed\n");
        return NULL;
    }

    // 帧率控制
    struct timespec ts;
    const long target_frame_ns = 33333333; // 30fps
    
    // 初始化线程本地turbojpeg实例
    if ((tjInstance = tjInitDecompress()) == NULL) {
        OPRINT("TurboJPEG init error: %s\n", tjGetErrorStr());
        return NULL;
    }

    while (!pglobal->stop) {
        clock_gettime(CLOCK_MONOTONIC, &ts);
        
        process_events();

        /* 获取JPEG数据 */
        pthread_mutex_lock(&pglobal->in[input_number].db);
        pthread_cond_wait(&pglobal->in[input_number].db_update,
                         &pglobal->in[input_number].db);
        
        size_t frame_size = pglobal->in[input_number].size;
        if (frame_size > MAX_FRAME_SIZE) {
            OPRINT("Frame too large: %zu > %d\n", frame_size, MAX_FRAME_SIZE);
            pthread_mutex_unlock(&pglobal->in[input_number].db);
            continue;
        }
        memcpy(jpeg_buf, pglobal->in[input_number].buf, frame_size);
        pthread_mutex_unlock(&pglobal->in[input_number].db);

        /* TurboJPEG解码 */
        int width, height, jpegSubsamp;
        if (tjDecompressHeader2(tjInstance, jpeg_buf, frame_size, 
                               &width, &height, &jpegSubsamp) != 0) {
            OPRINT("tjDecompressHeader2 error: %s\n", tjGetErrorStr());
            continue;
        }
        
        if (tjDecompress2(tjInstance, jpeg_buf, frame_size,  
                         rgb_buf, width, 0, height,
                         TJPF_RGB, TJFLAG_FASTDCT) != 0) {
            OPRINT("tjDecompress2 error: %s\n", tjGetErrorStr());
            continue;
        }

        /* 异步渲染（减少锁时间）*/
        if (init_sdl(width, height) == 0) {
            pthread_mutex_lock(&ctx_mutex);
            SDL_UpdateTexture(ctx.texture, NULL, rgb_buf, width * 3);
            SDL_RenderClear(ctx.renderer);
            SDL_RenderCopy(ctx.renderer, ctx.texture, NULL, NULL);
            SDL_RenderPresent(ctx.renderer);
            pthread_mutex_unlock(&ctx_mutex);
        }

        // 精确帧率控制
        ts.tv_nsec += target_frame_ns;
        if (ts.tv_nsec >= 1e9) {
            ts.tv_sec++;
            ts.tv_nsec -= 1e9;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
    }

    if (tjInstance) {
        tjDestroy(tjInstance);
        tjInstance = NULL;
    }

    free(jpeg_buf);
    free(rgb_buf);
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

     // 强制清理线程本地存储
    if (tjInstance) {
        tjDestroy(tjInstance);
        tjInstance = NULL;
     }

    return 0;
}