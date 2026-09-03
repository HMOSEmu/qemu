#include "qemu/osdep.h"
#include "harmony-pipe.h"
#include "websock-passthrough-pipe.h"
#include "qemu/iov.h"

#define WEBSOCK_PASSTHROUGH_PIPE_WAKE_READ  1
#define WEBSOCK_PASSTHROUGH_PIPE_WAKE_WRITE 2
#define WEBSOCK_PASSTHROUGH_PIPE_WAKE_POLL  4

typedef struct WebSockPath WebSockPath;

typedef struct {
    const HarmonyPipeFuncs    *vtbl;
    const VirtIOPipe          *pipe;

    char                      *path;
    Buffer                    input;
    HarmonyClient             *hc;
    unsigned                  flags;
} WebSockPassThroughPipe;

struct WebSockPath {
    const char                *path;
    QTAILQ_ENTRY(WebSockPath) next;
};

static QTAILQ_HEAD(, WebSockPath) paths;

static gboolean websock_passthrough_pipe_connect(HarmonyClient *hc, void *opaque) 
{
    WebSockPassThroughPipe *wp_pipe = opaque;

    if (wp_pipe->hc != NULL) {
        if (wp_pipe->hc->disconnecting) {
            harmony_server_disconnect_finish(wp_pipe->hc);
        } else {
            return FALSE;
        }
    }

    wp_pipe->hc = hc;

    if (wp_pipe->flags & WEBSOCK_PASSTHROUGH_PIPE_WAKE_POLL) {
        harmony_pipe_host_wake_write(wp_pipe->pipe);
        wp_pipe->flags &= ~WEBSOCK_PASSTHROUGH_PIPE_WAKE_POLL;
    }

    if (wp_pipe->flags & WEBSOCK_PASSTHROUGH_PIPE_WAKE_READ) {
        harmony_pipe_host_wake_read(wp_pipe->pipe);
    }

    if (wp_pipe->flags & WEBSOCK_PASSTHROUGH_PIPE_WAKE_WRITE) {
        harmony_pipe_host_wake_write(wp_pipe->pipe);
    }

    return TRUE;
}

static int websock_passthrough_pipe_read(HarmonyClient *hc, void *opaque, uint8_t *data, size_t len) 
{
    WebSockPassThroughPipe *wp_pipe = opaque;

    buffer_reserve(&wp_pipe->input, len);
    buffer_append(&wp_pipe->input, data, len);

    if (wp_pipe->flags & WEBSOCK_PASSTHROUGH_PIPE_WAKE_POLL) {
        harmony_pipe_host_wake_read(wp_pipe->pipe);
        wp_pipe->flags &= ~WEBSOCK_PASSTHROUGH_PIPE_WAKE_POLL;
    }

    if (wp_pipe->flags & WEBSOCK_PASSTHROUGH_PIPE_WAKE_READ) {
        harmony_pipe_host_wake_read(wp_pipe->pipe);
    }

    return len;
}

static void websock_passthrough_pipe_disconnect(HarmonyClient *hc, void *opaque) 
{
    WebSockPassThroughPipe *wp_pipe = opaque;

    wp_pipe->hc = NULL;
}

static int websock_passthrough_pipe_path_exists(const char *path)
{
    WebSockPath *ws_path;

    QTAILQ_FOREACH(ws_path, &paths, next) {
        if (!strcmp(ws_path->path, path)) { 
            return 1;
        }
    }

    return 0;
}

static void websock_passthrough_pipe_add_path(const char *path)
{
    WebSockPath *ws_path = g_new0(WebSockPath, 1);
    ws_path->path = path;
    QTAILQ_INSERT_TAIL(&paths, ws_path, next);
}

static void websock_passthrough_pipe_remove_path(const char *path)
{
    int found = 0;
    WebSockPath *ws_path;

    QTAILQ_FOREACH(ws_path, &paths, next) {
        if (!strcmp(ws_path->path, path)) { 
            found = 1;
            break;
        }
    }

    if (found) {
        QTAILQ_REMOVE(&paths, ws_path, next);
        g_free(ws_path);
    }
}

static void* websock_passthrough_pipe_create(const void *pipe, 
                                        const void *opaque, 
                                        const HarmonyPipeFuncs *funcs,
                                        const char *args)
{
    WebSockPassThroughPipe *wp_pipe = NULL;
    char *path;

    if (websock_passthrough_pipe_path_exists(args)) {
        return NULL;
    }

    wp_pipe = g_new0(WebSockPassThroughPipe, 1);
    path = g_strdup_printf("/%s", args);

    wp_pipe->vtbl = funcs;
    wp_pipe->pipe = pipe;
    wp_pipe->path = path;
    wp_pipe->hc = NULL;
    wp_pipe->flags = 0;
    
    buffer_init(&wp_pipe->input, "websock-passthrough-input/%s", path);

    websock_passthrough_pipe_add_path(path);
    harmony_server_add_path_handler(path, websock_passthrough_pipe_connect, 
                                websock_passthrough_pipe_read, 
                                websock_passthrough_pipe_disconnect, wp_pipe);
    
    return wp_pipe;
}

static void websock_passthrough_pipe_close(void *service_pipe)
{
    WebSockPassThroughPipe *wp_pipe = (WebSockPassThroughPipe*)service_pipe;

    if (wp_pipe->hc != NULL)
        harmony_server_disconnect_start(wp_pipe->hc);
    
    websock_passthrough_pipe_remove_path(wp_pipe->path); 
    harmony_server_remove_path_handler(wp_pipe->path);

    buffer_free(&wp_pipe->input);

    g_free(wp_pipe->path);
    g_free(wp_pipe);
}

static int websock_passthrough_pipe_send_buffers(void *service_pipe,
                                            const struct iovec *buffers,
                                            int num_buffers)
{
    WebSockPassThroughPipe *wp_pipe = (WebSockPassThroughPipe*)service_pipe;
    const struct iovec *buff = buffers;
    const struct iovec *buff_end = buff + num_buffers;
    int len;

    wp_pipe->flags &= ~WEBSOCK_PASSTHROUGH_PIPE_WAKE_WRITE;

    if (wp_pipe->hc == NULL) {
        return VIRTIO_PIPE_ERROR_AGAIN;
    }

    if (wp_pipe->hc->disconnecting) {
        harmony_server_disconnect_finish(wp_pipe->hc);
        return VIRTIO_PIPE_ERROR_AGAIN;
    }

    len = 0;
    for (; buff < buff_end; ++buff) {
        len += harmony_server_write(wp_pipe->hc, buff->iov_base, buff->iov_len);
    }

    return len;
}

static int websock_passthrough_pipe_recv_buffers(void *service_pipe,
                                            struct iovec *buffers,
                                            int num_buffers)
{
    WebSockPassThroughPipe *wp_pipe = (WebSockPassThroughPipe*)service_pipe;
    const struct iovec *buff = buffers;
    const struct iovec *buff_end = buff + num_buffers;
    int len, remain;

    wp_pipe->flags &= ~WEBSOCK_PASSTHROUGH_PIPE_WAKE_READ;

    if (wp_pipe->hc == NULL) {
        return VIRTIO_PIPE_ERROR_AGAIN;
    }

    if (buffer_empty(&wp_pipe->input)) {
        return VIRTIO_PIPE_ERROR_AGAIN;
    }

    len = 0;
    remain = wp_pipe->input.offset;
    for (; buff < buff_end; ++buff) {
        if (buff->iov_len > remain) {
            memcpy(buff->iov_base, wp_pipe->input.buffer, remain);
            buffer_advance(&wp_pipe->input, remain);
            len += remain;
            remain = 0;
            break;
        } else {
            memcpy(buff->iov_base, wp_pipe->input.buffer, buff->iov_len);
            buffer_advance(&wp_pipe->input, buff->iov_len);
            len += buff->iov_len;
            remain -= buff->iov_len;
        }
    }

    return len;
}

static unsigned websock_passthrough_pipe_poll(void *service_pipe)
{
    WebSockPassThroughPipe *wp_pipe = (WebSockPassThroughPipe*)service_pipe;
    unsigned ret = 0;

    wp_pipe->flags |= WEBSOCK_PASSTHROUGH_PIPE_WAKE_POLL;

    ret |= VIRTIO_PIPE_POLL_OUT;

    if (!buffer_empty(&wp_pipe->input))
        ret |= VIRTIO_PIPE_POLL_IN;

    return ret;
}

static void websock_passthrough_pipe_wake_read(void *service_pipe)
{
    WebSockPassThroughPipe *wp_pipe = (WebSockPassThroughPipe*)service_pipe;

    wp_pipe->flags |= WEBSOCK_PASSTHROUGH_PIPE_WAKE_READ;
}

static void websock_passthrough_pipe_wake_write(void *service_pipe)
{
    WebSockPassThroughPipe *wp_pipe = (WebSockPassThroughPipe*)service_pipe;

    wp_pipe->flags |= WEBSOCK_PASSTHROUGH_PIPE_WAKE_WRITE;
}

static const HarmonyPipeFuncs websock_passthrough_pipe_funcs = {
    .create = websock_passthrough_pipe_create,
    .close = websock_passthrough_pipe_close,
    .send_buffers = websock_passthrough_pipe_send_buffers,
    .recv_buffers = websock_passthrough_pipe_recv_buffers,
    .poll = websock_passthrough_pipe_poll,
    .wake_read = websock_passthrough_pipe_wake_read,
    .wake_write = websock_passthrough_pipe_wake_write,
};

void harmony_pipe_add_service_websock_passthrough(void)
{
    QTAILQ_INIT(&paths);

    harmony_pipe_add_service("websock-passthrough", NULL, 
                        &websock_passthrough_pipe_funcs);
}
