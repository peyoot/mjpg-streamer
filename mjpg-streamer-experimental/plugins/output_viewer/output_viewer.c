/*******************************************************************************
#                                                                              #
#      MJPG-streamer GStreamer/Wayland Viewer 插件（修复版）                   #
#                                                                              #
*******************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
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
    guint64 frame_count;
} PluginContext;

static PluginContext ctx = {0};
static pthread_t worker_thread;
static globals *pglobal;
static int input_number = 0;

/* GStreamer总线消息处理 */
static gboolean bus_callback(GstBus *bus, GstMessage *msg, gpointer data) {
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS:
        DEBUG("Pipeline EOS");
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
    case GST_MESSAGE_STATE_CHANGED: {
        GstState old, new, pending;
        gst_message_parse_state_changed(msg, &old, &new, &pending);
        DEBUG("State changed: %s -> %s",
            gst_element_state_get_name(old),
            gst_element_state_get_name(new));
        break;
    }
    default:
        break;
    }
    return TRUE;
}

/* 初始化/重建GStreamer管道 */
static int init_gstreamer(int width, int height, int fps) {
    if (width <= 0 || height <= 0 || fps <= 0) {
        DEBUG("Invalid parameters: %dx%d@%d", width, height, fps);
        return -1;
    }

    pthread_mutex_lock(&ctx.lock);
    
    /* 如果参数变化需要重建管道 */
    if (ctx.initialized && (ctx.width != width || ctx.height != height || ctx.fps != fps)) {
        DEBUG("Reinitializing pipeline for new resolution");
        gst_element_send_event(ctx.pipeline, gst_event_new_eos());
        gst_element_set_state(ctx.pipeline, GST_STATE_NULL);
        gst_object_unref(ctx.pipeline);
        ctx.initialized = 0;
    }

    if (!ctx.initialized) {
        if (!gst_is_initialized()) {
            gst_init(NULL, NULL);
        }

        /* 构建硬件加速流水线 */
        char pipeline_str[512];
        snprintf(pipeline_str, sizeof(pipeline_str),
            "appsrc name=src is-live=true format=3 do-timestamp=true ! "
            "image/jpeg,width=%d,height=%d,framerate=%d/1 ! "
            "jpegparse ! v4l2jpegdec ! videoconvert ! "
            "waylandsink name=wsink sync=false", 
            width, height, fps);

        GError *err = NULL;
        ctx.pipeline = gst_parse_launch(pipeline_str, &err);
        if (!ctx.pipeline) {
            DEBUG("Pipeline creation failed: %s", err ? err->message : "Unknown error");
            if (err) g_error_free(err);
            pthread_mutex_unlock(&ctx.lock);
            return -1;
        }

        /* 获取元件引用并验证 */
        ctx.app_src = gst_bin_get_by_name(GST_BIN(ctx.pipeline), "src");
        ctx.wayland_sink = gst_bin_get_by_name(GST_BIN(ctx.pipeline), "wsink");
        
        if (!ctx.app_src || !ctx.wayland_sink) {
            DEBUG("Failed to get pipeline elements");
            if (ctx.pipeline) {
                gst_object_unref(ctx.pipeline);
                ctx.pipeline = NULL;
            }
            pthread_mutex_unlock(&ctx.lock);
            return -1;
        }

        /* 配置appsrc参数 */
        g_object_set(ctx.app_src,
            "stream-type", 0,          // GST_APP_STREAM_TYPE_STREAM
            "format", GST_FORMAT_TIME, 
            "block", TRUE,
            "emit-signals", FALSE,
            NULL);

        /* 总线监视 */
        GstBus *bus = gst_element_get_bus(ctx.pipeline);
        gst_bus_add_watch(bus, bus_callback, NULL);
        gst_object_unref(bus);

        /* 启动管道 */
        GstStateChangeReturn ret = gst_element_set_state(ctx.pipeline, GST_STATE_PLAYING);
        if (ret == GST_STATE_CHANGE_FAILURE) {
            DEBUG("Failed to start pipeline");
            gst_object_unref(ctx.pipeline);
            ctx.pipeline = NULL;
            pthread_mutex_unlock(&ctx.lock);
            return -1;
        }

        ctx.width = width;
        ctx.height = height;
        ctx.fps = fps;
        ctx.initialized = 1;
        ctx.frame_count = 0;
        
        DEBUG("Pipeline initialized for %dx%d@%dfps", width, height, fps);
    }
    
    pthread_mutex_unlock(&ctx.lock);
    return 0;
}

/* 工作线程 */
static void *gst_worker(void *arg) {
    GstFlowReturn ret = GST_FLOW_OK;
    
    while (!pglobal->stop) {
        pthread_mutex_lock(&pglobal->in[input_number].db);
        pthread_cond_wait(&pglobal->in[input_number].db_update,
                         &pglobal->in[input_number].db);
        
        int width = pglobal->in[input_number].width;
        int height = pglobal->in[input_number].height;
        int fps = pglobal->in[input_number].fps;
        size_t size = pglobal->in[input_number].size;
        void *data = pglobal->in[input_number].buf;

        /* 初始化/更新管道 */
        if (init_gstreamer(width, height, fps) != 0) {
            pthread_mutex_unlock(&pglobal->in[input_number].db);
            usleep(100000); // 100ms延时
            continue;
        }

        /* 创建并推送缓冲区 */
        GstBuffer *buffer = gst_buffer_new_wrapped_full(
            GST_MEMORY_FLAG_READONLY,
            data, size, 0, size, NULL, NULL);
        
        /* 生成时间戳 */
        GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(ctx.frame_count++, GST_SECOND, ctx.fps);
        
        pthread_mutex_lock(&ctx.lock);
        g_signal_emit_by_name(ctx.app_src, "push-buffer", buffer, &ret);
        pthread_mutex_unlock(&ctx.lock);
        
        gst_buffer_unref(buffer);
        pthread_mutex_unlock(&pglobal->in[input_number].db);

        if (ret != GST_FLOW_OK) {
            DEBUG("Push buffer failed: %s", gst_flow_get_name(ret));
            break;
        }
    }
    return NULL;
}

/* 插件初始化 */
int output_init(output_parameter *param) {
    int opt;
    char **argv = param->argv;
    int argc = param->argc;
    
    /* 重置getopt */
    optind = 1;
    while ((opt = getopt(argc, argv, "i:")) != -1) {
        switch (opt) {
        case 'i':
            input_number = atoi(optarg);
            break;
        default:
            DEBUG("Unknown option: %c", opt);
            return -1;
        }
    }
    
    pglobal = param->global;
    if (input_number >= pglobal->incnt) {
        DEBUG("Invalid input number: %d", input_number);
        return -1;
    }
    
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
        
        if (ctx.app_src) {
            gst_object_unref(ctx.app_src);
            ctx.app_src = NULL;
        }
        if (ctx.wayland_sink) {
            gst_object_unref(ctx.wayland_sink);
            ctx.wayland_sink = NULL;
        }
        if (ctx.pipeline) {
            gst_object_unref(ctx.pipeline);
            ctx.pipeline = NULL;
        }
        
        ctx.initialized = 0;
        DEBUG("Pipeline destroyed");
    }
    pthread_mutex_unlock(&ctx.lock);
    pthread_mutex_destroy(&ctx.lock);
    return 0;
}