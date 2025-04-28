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

#define OUTPUT_PLUGIN_NAME "Simple Viewer Plugin"

static pthread_t worker;
static globals *pglobal;
static int input_number = 0;

/* 简化的上下文结构 */
typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    int width;
    int height;
} DisplayContext;

/* 核心显示函数 */
static void show_image(unsigned char *buffer, int width, int height) {
    static DisplayContext ctx = {0};
    
    // 首次运行初始化
    if(!ctx.window) {
        SDL_Init(SDL_INIT_VIDEO);
        ctx.window = SDL_CreateWindow("Camera View",
                                      SDL_WINDOWPOS_UNDEFINED,
                                      SDL_WINDOWPOS_UNDEFINED,
                                      width, height,
                                      SDL_WINDOW_SHOWN);
        ctx.renderer = SDL_CreateRenderer(ctx.window, -1, 0);
        ctx.texture = SDL_CreateTexture(ctx.renderer,
                                       SDL_PIXELFORMAT_RGB24,
                                       SDL_TEXTUREACCESS_STREAMING,
                                       width, height);
    }

    // 更新显示
    if(ctx.texture) {
        SDL_UpdateTexture(ctx.texture, NULL, buffer, width * 3);
        SDL_RenderClear(ctx.renderer);
        SDL_RenderCopy(ctx.renderer, ctx.texture, NULL, NULL);
        SDL_RenderPresent(ctx.renderer);
    }
}

/* 工作线程 */
static void *worker_thread(void *arg) {
    unsigned char *frame = NULL;
    int frame_size = 0;

    // 分配帧缓存
    if((frame = malloc(4096 * 1024)) == NULL) {
        OPRINT("Memory allocation failed\n");
        return NULL;
    }

    while(!pglobal->stop) {
        // 获取最新帧
        pthread_mutex_lock(&pglobal->in[input_number].db);
        pthread_cond_wait(&pglobal->in[input_number].db_update, 
                         &pglobal->in[input_number].db);
        frame_size = pglobal->in[input_number].size;
        memcpy(frame, pglobal->in[input_number].buf, frame_size);
        pthread_mutex_unlock(&pglobal->in[input_number].db);

        // 解码JPEG
        struct jpeg_decompress_struct cinfo;
        struct jpeg_error_mgr jerr;
        unsigned char *rgb_buffer = NULL;

        cinfo.err = jpeg_std_error(&jerr);
        jpeg_create_decompress(&cinfo);
        jpeg_mem_src(&cinfo, frame, frame_size);
        jpeg_read_header(&cinfo, TRUE);
        jpeg_start_decompress(&cinfo);

        // 分配RGB缓冲区
        rgb_buffer = malloc(cinfo.output_width * cinfo.output_height * 3);
        while(cinfo.output_scanline < cinfo.output_height) {
            unsigned char *row = rgb_buffer + 
                cinfo.output_scanline * cinfo.output_width * 3;
            jpeg_read_scanlines(&cinfo, &row, 1);
        }

        // 显示图像
        show_image(rgb_buffer, cinfo.output_width, cinfo.output_height);

        // 清理
        free(rgb_buffer);
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
    }

    free(frame);
    return NULL;
}

/* 插件接口 */
int output_init(output_parameter *param) {
    int opt;
    // 修复的getopt循环
    while((opt = getopt(param->argc, param->argv, "i:")) != -1) {
        switch(opt) {
            case 'i':
                input_number = atoi(optarg);
                break;
            default:
                break;
        }
    }
    pglobal = param->global;
    return 0;
}

int output_run(int id) {
    pthread_create(&worker, NULL, worker_thread, NULL);
    pthread_detach(worker);
    return 0;
}

int output_stop(int id) {
    pglobal->stop = 1;
    return 0;
}

int output_cmd(int plugin, unsigned int control, unsigned int group, int value) {
    return 0;
}
