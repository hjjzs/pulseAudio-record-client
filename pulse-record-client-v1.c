#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pulse/pulseaudio.h>
#include <libwebsockets.h>

// 全局变量
static pa_stream *stream;
static pa_context *context;
static struct lws_context *ws_context;
static struct lws_protocols protocols[];

// 客户端列表
typedef struct client {
    struct lws *wsi;
    struct client *next;
} client_t;

static client_t *clients = NULL;
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

// 将音频数据发送给所有连接的客户端
void broadcast_audio(const void *data, size_t size) {
    pthread_mutex_lock(&clients_mutex);
    client_t *current = clients;
    while (current) {
        // 为了发送二进制数据，需要在每个消息前添加 LWS_PRE 个字节的空间
        unsigned char buffer[LWS_PRE + size];
        memcpy(&buffer[LWS_PRE], data, size);

        lws_write(current->wsi, &buffer[LWS_PRE], size, LWS_WRITE_BINARY);
        current = current->next;
    }
    pthread_mutex_unlock(&clients_mutex);
}

// 读取回调函数，将音频数据通过 WebSocket 发送
void read_callback(pa_stream *s, size_t length, void *userdata) {
    const void *data;
    size_t size;

    // 从流中获取音频数据
    pa_stream_peek(s, &data, &size);

    if (data) {
        // 通过 WebSocket 发送音频数据
        broadcast_audio(data, size);

        // 丢弃已读取的数据
        pa_stream_drop(s);
    }
}

// PulseAudio 上下文状态回调
void state_callback(pa_context *c, void *userdata) {
    if (pa_context_get_state(c) == PA_CONTEXT_READY) {
        // 定义音频格式：16位，44100Hz，立体声
        pa_sample_spec ss = {
            .format = PA_SAMPLE_S16LE,
            .rate = 44100,
            .channels = 2
        };

        // 设置缓冲区属性以减少延迟
        pa_buffer_attr buffer_attr = {
            .fragsize = pa_usec_to_bytes(20 * PA_USEC_PER_MSEC, &ss),
            .tlength = pa_usec_to_bytes(20 * PA_USEC_PER_MSEC, &ss),
            .maxlength = (uint32_t)-1,
            .minreq = (uint32_t)-1,
            .prebuf = (uint32_t)-1
        };

        // 创建录音流
        stream = pa_stream_new(c, "Audio Stream", &ss, NULL);

        // 设置读取回调
        pa_stream_set_read_callback(stream, read_callback, NULL);

        // 连接到 PulseAudio 的 auto_null.monitor 设备
        pa_stream_connect_record(stream, "auto_null.monitor", &buffer_attr, PA_STREAM_ADJUST_LATENCY);
    }
}

// WebSocket 回调函数
static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len) {
    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED: {
            // 新的客户端连接
            client_t *new_client = malloc(sizeof(client_t));
            new_client->wsi = wsi;
            new_client->next = NULL;

            pthread_mutex_lock(&clients_mutex);
            new_client->next = clients;
            clients = new_client;
            pthread_mutex_unlock(&clients_mutex);

            printf("Client connected\n");
            break;
        }
        case LWS_CALLBACK_CLOSED: {
            // 客户端断开连接
            pthread_mutex_lock(&clients_mutex);
            client_t **current = &clients;
            while (*current) {
                if ((*current)->wsi == wsi) {
                    client_t *to_delete = *current;
                    *current = (*current)->next;
                    free(to_delete);
                    break;
                }
                current = &((*current)->next);
            }
            pthread_mutex_unlock(&clients_mutex);

            printf("Client disconnected\n");
            break;
        }
        default:
            break;
    }
    return 0;
}

// WebSocket 协议列表
static struct lws_protocols protocols[] = {
    {
        .name = "audio-protocol",
        .callback = ws_callback,
        .per_session_data_size = 0,
        .rx_buffer_size = 0,
    },
    { NULL, NULL, 0, 0 } // 必须以 NULL 结尾
};

// WebSocket 服务线程
void *websocket_service(void *arg) {
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof info);

    info.port = 8080; // WebSocket 服务器监听的端口
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;

    ws_context = lws_create_context(&info);
    if (!ws_context) {
        fprintf(stderr, "WebSocket context creation failed\n");
        return NULL;
    }

    printf("WebSocket server started on port %d\n", info.port);

    while (1) {
        lws_service(ws_context, 1000);
    }

    lws_context_destroy(ws_context);
    return NULL;
}

int main() {
    // 启动 WebSocket 服务线程
    pthread_t ws_thread;
    pthread_create(&ws_thread, NULL, websocket_service, NULL);

    // 初始化 PulseAudio 主循环
    pa_mainloop *mainloop = pa_mainloop_new();
    pa_mainloop_api *api = pa_mainloop_get_api(mainloop);

    // 创建 PulseAudio 上下文
    context = pa_context_new(api, "Low Latency Example");

    // 设置上下文状态回调
    pa_context_set_state_callback(context, state_callback, NULL);

    // 连接到 PulseAudio 服务器
    pa_context_connect(context, NULL, PA_CONTEXT_NOFAIL, NULL);

    // 运行主循环
    pa_mainloop_run(mainloop, NULL);

    // 清理资源
    pa_context_disconnect(context);
    pa_context_unref(context);
    pa_stream_unref(stream);
    pa_mainloop_free(mainloop);

    // 停止 WebSocket 服务线程
    pthread_cancel(ws_thread);
    pthread_join(ws_thread, NULL);

    return 0;
}
