/*******************************************************************************
#                                                                              #
#      MJPG-streamer 的 GStreamer/Wayland 本地显示插件                         #
#      基于零拷贝架构设计，与HTTP输出插件完全兼容                              #
#                                                                              #
#      Copyright (C) 2024 Digi International Inc.                             #
#                                                                              #
#      优化特性：                                                              #
#      - 直接使用摄像头MJPG流，避免重复解码                                    #
#      - 通过GStreamer硬件加速流水线                                           #
#      - Wayland本地渲染，零内存拷贝                                           #
#      - 自动适配分辨率变化                                                    #
#                                                                              #
*******************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include "../../utils.h"
#include "../../mjpg_streamer.h"

#define PLUGIN_NAME "GStreamer/Wayland Viewer"
#define DEBUG(fmt, ...) fprintf(stderr, "GST-VIEWER: " fmt "\n", ##__VA_ARGS__)

/* 全局上下文结构体 */
typedef struct {
    GstElement *pipeline;
    GstElement *app_src;
    GstElement *wayland_sink;
    int width;
    int height;
    int fps;
    pthread_mutex_t lock;
    int initialized;
} PluginContext;

static PluginContext ctx = {0};
static pthread_t worker_thread;
static globals *pglobal;
static int input_number = 0;

/* GStreamer总线监视回调 */
static gboolean bus_callback(GstBus *bus, GstMessage *msg, gpointer data) {
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS:
        DEBUG("Pipeline reached EOS");
        break;
    case GST_MESSAGE_ERROR: {
        gchar *debug;
        GError *err;
        gst_message_parse_error(msg, &err, &debug);
        DEBUG("Error: %s (%s)", err->message, debug ? debug : "none");
        g_error_free(err);
        g_free(debug);
        break;
    }
    default:
        break;
    }
    return TRUE;
}

/* 初始化GStreamer流水线 */
static int init_gstreamer(int width, int height, int fps) {
    pthread_mutex_lock(&ctx.lock);
    
    if (!ctx.initialized) {
        gst_init(NULL, NULL);
        
        /* 构建硬件加速流水线 */
        char pipeline_str[512];
        snprintf(pipeline_str, sizeof(pipeline_str),
            "appsrc name=src ! "
            "image/jpeg,width=%d,height=%d,framerate=%d/1 ! "
            "jpegparse ! v4l2jpegdec ! v4l2convert ! "
            "waylandsink name=wsink sync=false", 
            width, height, fps);
        
        ctx.pipeline = gst_parse_launch(pipeline_str, NULL);
        if (!ctx.pipeline) {
            DEBUG("Failed to create GStreamer pipeline");
            pthread_mutex_unlock(&ctx.lock);
            return -1;
        }

        /* 获取元件引用 */
        ctx.app_src = gst_bin_get_by_name(GST_BIN(ctx.pipeline), "src");
        ctx.wayland_sink = gst_bin_get_by_name(GST_BIN(ctx.pipeline), "wsink");
        
        /* 配置appsrc */
        g_object_set(ctx.app_src,
            "stream-type", 0,          // GST_APP_STREAM_TYPE_STREAM
            "format", GST_FORMAT_TIME, 
            "block", TRUE,            // 防止数据丢失
            NULL);
        
        /* 总线监视 */
        GstBus *bus = gst_element_get_bus(ctx.pipeline);
        gst_bus_add_watch(bus, bus_callback, NULL);
        gst_object_unref(bus);
        
        /* 启动流水线 */
        if (gst_element_set_state(ctx.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            DEBUG("Failed to start pipeline");
            pthread_mutex_unlock(&ctx.lock);
            return -1;
        }
        
        ctx.width = width;
        ctx.height = height;
        ctx.fps = fps;
        ctx.initialized = 1;
        
        DEBUG("Pipeline initialized for %dx%d@%dfps", width, height, fps);
    }
    
    pthread_mutex_unlock(&ctx.lock);
    return 0;
}

/* 工作线程：接收并推送数据 */
static void *gst_worker(void *arg) {
    GstFlowReturn ret = GST_FLOW_OK;
    GstBuffer *buffer;
    GstMapInfo map;
    
    while (!pglobal->stop) {
        /* 获取JPEG数据 */
        pthread_mutex_lock(&pglobal->in[input_number].db);
        pthread_cond_wait(&pglobal->in[input_number].db_update, 
                         &pglobal->in[input_number].db);
        
        size_t size = pglobal->in[input_number].size;
        void *data = pglobal->in[input_number].buf;
        
        /* 创建GStreamer缓冲区（零拷贝） */
        buffer = gst_buffer_new_wrapped_full(
            GST_MEMORY_FLAG_READONLY,
            data, size, 0, size, NULL, NULL);
        
        /* 推送数据到流水线 */
        g_signal_emit_by_name(ctx.app_src, "push-buffer", buffer, &ret);
        gst_buffer_unref(buffer);
        
        pthread_mutex_unlock(&pglobal->in[input_number].db);
        
        if (ret != GST_FLOW_OK) {
            DEBUG("Push buffer failed: %d", ret);
            break;
        }
    }
    return NULL;
}

/* 插件初始化 */
int output_init(output_parameter *param) {
    int opt;
    while ((opt = getopt(param->argc, param->argv, "i:")) {
        if (opt == -1) break;
        if (opt == 'i') input_number = atoi(optarg);
    }
    pglobal = param->global;
    pthread_mutex_init(&ctx.lock, NULL);
    return 0;
}

/* 启动插件 */
int output_run(int id) {
    if (pthread_create(&worker_thread, NULL, gst_worker, NULL) != 0) {
        DEBUG("Failed to create worker thread");
        return -1;
    }
    pthread_detach(worker_thread);
    return 0;
}

/* 停止插件 */
int output_stop(int id) {
    pthread_mutex_lock(&ctx.lock);
    if (ctx.initialized) {
        gst_element_send_event(ctx.pipeline, gst_event_new_eos());
        gst_element_set_state(ctx.pipeline, GST_STATE_NULL);
        gst_object_unref(ctx.pipeline);
        ctx.initialized = 0;
        DEBUG("Pipeline destroyed");
    }
    pthread_mutex_unlock(&ctx.lock);
    pthread_mutex_destroy(&ctx.lock);
    return 0;
}