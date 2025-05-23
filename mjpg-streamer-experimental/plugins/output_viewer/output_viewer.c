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
#include <getopt.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include "../../utils.h"
#include "../../mjpg_streamer.h"

#define PLUGIN_NAME "GStreamer/Wayland Viewer"
#define DEFAULT_WIDTH 640
#define DEFAULT_HEIGHT 480
#define DEFAULT_FPS 30
#define DEBUG(fmt, ...) fprintf(stderr, "GST-VIEWER: " fmt "\n", ##__VA_ARGS__)

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

/* 函数原型声明 */
static gboolean bus_callback(GstBus *bus, GstMessage *msg, gpointer data);
static int init_gstreamer(int width, int height, int fps);
static void cleanup_resources(void);
static void *gst_worker(void *arg);

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
        if (GST_MESSAGE_SRC(msg) == GST_OBJECT(ctx.pipeline)) {
            GstState old, new, pending;
            gst_message_parse_state_changed(msg, &old, &new, &pending);
            DEBUG("State: %s -> %s", 
                gst_element_state_get_name(old),
                gst_element_state_get_name(new));
        }
        break;
    }
    default:
        break;
    }
    return TRUE;
}

static int init_gstreamer(int width, int height, int fps) {
    if (width <= 0 || height <= 0 || fps <= 0) {
        DEBUG("Invalid params: %dx%d@%d", width, height, fps);
        return -1;
    }

    pthread_mutex_lock(&ctx.lock);
    
    if (ctx.initialized) {
        if (ctx.width == width && ctx.height == height && ctx.fps == fps) {
            pthread_mutex_unlock(&ctx.lock);
            return 0;
        }
        DEBUG("Reconfiguring pipeline");
        gst_element_send_event(ctx.pipeline, gst_event_new_eos());
        gst_element_set_state(ctx.pipeline, GST_STATE_NULL);
        gst_object_unref(ctx.pipeline);
        ctx.initialized = 0;
    }

    if (!gst_is_initialized()) {
        gst_init(NULL, NULL);
    }

    char pipeline_str[512];
    snprintf(pipeline_str, sizeof(pipeline_str),
        "appsrc name=src is-live=true format=3 do-timestamp=true ! "
        "image/jpeg,width=%d,height=%d,framerate=%d/1 ! "
        "jpegparse ! jpegdec ! videoconvert ! "
        "waylandsink name=wsink sync=false",
        width, height, fps);

    GError *err = NULL;
    ctx.pipeline = gst_parse_launch(pipeline_str, &err);
    if (!ctx.pipeline) {
        DEBUG("Pipeline create failed: %s", err ? err->message : "Unknown error");
        if (err) g_error_free(err);
        pthread_mutex_unlock(&ctx.lock);
        return -1;
    }

    ctx.app_src = gst_bin_get_by_name(GST_BIN(ctx.pipeline), "src");
    ctx.wayland_sink = gst_bin_get_by_name(GST_BIN(ctx.pipeline), "wsink");
    
    if (!ctx.app_src || !ctx.wayland_sink) {
        DEBUG("Failed to get elements");
        cleanup_resources();
        pthread_mutex_unlock(&ctx.lock);
        return -1;
    }

    g_object_set(ctx.app_src,
        "stream-type", 0,
        "format", GST_FORMAT_TIME,
        "block", TRUE,
        "emit-signals", FALSE,
        NULL);

    GstBus *bus = gst_element_get_bus(ctx.pipeline);
    gst_bus_add_watch(bus, bus_callback, NULL);
    gst_object_unref(bus);

    if (gst_element_set_state(ctx.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        DEBUG("Failed to start pipeline");
        cleanup_resources();
        pthread_mutex_unlock(&ctx.lock);
        return -1;
    }

    ctx.width = width;
    ctx.height = height;
    ctx.fps = fps;
    ctx.initialized = 1;
    ctx.frame_count = 0;

    DEBUG("Pipeline ready: %dx%d@%dfps", width, height, fps);
    pthread_mutex_unlock(&ctx.lock);
    return 0;
}

static void cleanup_resources(void) {
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
}

static void *gst_worker(void *arg) {
    while (!pglobal->stop) {
        pthread_mutex_lock(&pglobal->in[input_number].db);
        pthread_cond_wait(&pglobal->in[input_number].db_update,
                         &pglobal->in[input_number].db);

        int width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT, fps = DEFAULT_FPS;
        
        /* 解析输入参数 */
        input_parameter *in_param = &pglobal->in[input_number].param;
        for (int i = 0; i < in_param->argc; i++) {
            if (strcmp(in_param->argv[i], "-r") == 0 && (i+1) < in_param->argc) {
                sscanf(in_param->argv[i+1], "%dx%d", &width, &height);
            }
            if (strcmp(in_param->argv[i], "-f") == 0 && (i+1) < in_param->argc) {
                fps = atoi(in_param->argv[i+1]);
            }
        }

        if (init_gstreamer(width, height, fps) != 0) {
            pthread_mutex_unlock(&pglobal->in[input_number].db);
            usleep(100000);
            continue;
        }

        GstBuffer *buffer = gst_buffer_new_wrapped_full(
            GST_MEMORY_FLAG_READONLY,
            pglobal->in[input_number].buf,
            pglobal->in[input_number].size,
            0,
            pglobal->in[input_number].size,
            NULL, NULL);

        GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(ctx.frame_count++, GST_SECOND, ctx.fps);

        GstFlowReturn ret;
        pthread_mutex_lock(&ctx.lock);
        g_signal_emit_by_name(ctx.app_src, "push-buffer", buffer, &ret);
        pthread_mutex_unlock(&ctx.lock);

        gst_buffer_unref(buffer);
        pthread_mutex_unlock(&pglobal->in[input_number].db);

        if (ret != GST_FLOW_OK) {
            DEBUG("Buffer push failed: %s", gst_flow_get_name(ret));
            break;
        }
    }
    return NULL;
}

int output_init(output_parameter *param) {
    int opt;
    static struct option long_options[] = {
        {"input",   required_argument, 0, 'i'},
        {"width",   required_argument, 0, 'w'},
        {"height",  required_argument, 0, 'h'},
        {"fps",     required_argument, 0, 'f'},
        {0, 0, 0, 0}
    };

    int width = DEFAULT_WIDTH;
    int height = DEFAULT_HEIGHT;
    int fps = DEFAULT_FPS;

    optind = 1;
    while ((opt = getopt_long(param->argc, param->argv, "i:w:h:f:", long_options, NULL)) != -1) {
        switch (opt) {
        case 'i':
            input_number = atoi(optarg);
            break;
        case 'w':
            width = atoi(optarg);
            break;
        case 'h':
            height = atoi(optarg);
            break;
        case 'f':
            fps = atoi(optarg);
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

int output_run(int id) {
    if (pthread_create(&worker_thread, NULL, gst_worker, NULL) != 0) {
        DEBUG("Worker thread create failed");
        return -1;
    }
    pthread_detach(worker_thread);
    return 0;
}

int output_stop(int id) {
    pthread_mutex_lock(&ctx.lock);
    if (ctx.initialized) {
        gst_element_send_event(ctx.pipeline, gst_event_new_eos());
        gst_element_set_state(ctx.pipeline, GST_STATE_NULL);
        cleanup_resources();
        DEBUG("Pipeline shutdown");
    }
    pthread_mutex_unlock(&ctx.lock);
    pthread_mutex_destroy(&ctx.lock);
    return 0;
}