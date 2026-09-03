#include "qemu/osdep.h"
#include "harmony-pipe.h"
#include "audio-pipe.h"
#include "qemu/iov.h"

#define AUDIO_PIPE_WAKE_READ  1
#define AUDIO_PIPE_WAKE_WRITE 2
#define AUDIO_PIPE_WAKE_POLL  3

typedef struct AudioClient {
    HarmonyClient              *hc;
    QTAILQ_ENTRY(AudioClient)  next;
} AudioClient;

typedef struct AudioPipe {
    const HarmonyPipeFuncs     *vtbl;
    const VirtIOPipe           *pipe;

    unsigned                   flags;
    QTAILQ_ENTRY(AudioPipe)    next;
} AudioPipe;

typedef struct {
    QTAILQ_HEAD(, AudioPipe)   pipes;
    QTAILQ_HEAD(, AudioClient) clients;
    Buffer                     input;
} AudioHandler;

static AudioHandler *audio_handler = NULL;

static gboolean audio_pipe_connect(HarmonyClient *hc, void *opaque) 
{
    AudioHandler *ahandler = opaque;

    AudioClient *client = g_new0(AudioClient, 1);
    client->hc = hc;

    QTAILQ_INSERT_TAIL(&ahandler->clients, client, next);

    return TRUE;
}

static int audio_pipe_read(HarmonyClient *hc, void *opaque, uint8_t *data, size_t len) 
{
    AudioHandler *ahandler = opaque;
    AudioPipe *apipe;

    buffer_reset(&ahandler->input);
    buffer_reserve(&ahandler->input, len);
    buffer_append(&ahandler->input, data, len);

    QTAILQ_FOREACH(apipe, &ahandler->pipes, next) {
        if (apipe->flags & AUDIO_PIPE_WAKE_POLL) {
            harmony_pipe_host_wake_read(apipe->pipe);
            apipe->flags &= ~AUDIO_PIPE_WAKE_POLL;
        }

        if (apipe->flags & AUDIO_PIPE_WAKE_READ) {
            harmony_pipe_host_wake_read(apipe->pipe);
        }
    }

    return len;
}

static void audio_pipe_disconnect(HarmonyClient *hc, void *opaque) 
{
    AudioHandler *ahandler = opaque;
    AudioClient *client;
    int found = 0;

    QTAILQ_FOREACH(client, &ahandler->clients, next) {
        if (client->hc == hc) {
            found = 1;
            break;
        }
    }

    if (found) {
        QTAILQ_REMOVE(&ahandler->clients, client, next);
        g_free(client);
    }
}

static void* audio_pipe_create(const void *pipe, 
                        const void *opaque, 
                        const HarmonyPipeFuncs *funcs,
                        const char *args)
{
    AudioPipe *apipe = g_new0(AudioPipe, 1);
    apipe->pipe = pipe;
    apipe->vtbl = funcs;
    apipe->flags = 0;

    QTAILQ_INSERT_TAIL(&audio_handler->pipes, apipe, next);
    
    return apipe;
}

static void audio_pipe_close(void *service_pipe)
{
    AudioPipe *apipe = (AudioPipe*)service_pipe;

    QTAILQ_REMOVE(&audio_handler->pipes, apipe, next);
    g_free(apipe);
}

static int audio_pipe_send_buffers(void *service_pipe,
                            const struct iovec *buffers,
                            int num_buffers)
{
    AudioPipe *apipe = (AudioPipe*)service_pipe;
    const struct iovec *buff = buffers;
    const struct iovec *buff_end = buff + num_buffers;
    AudioClient *client, *next_client;
    int len;

    apipe->flags &= ~AUDIO_PIPE_WAKE_WRITE;

    len = 0;
    for (; buff < buff_end; ++buff) {
        len += buff->iov_len;
    }

    if (QTAILQ_EMPTY(&audio_handler->clients)) {
        return len;
    }

    buff = buffers;
    for (; buff < buff_end; ++buff) {
        QTAILQ_FOREACH_SAFE(client, &audio_handler->clients, next, next_client) {
            if (client->hc->disconnecting) {
                harmony_server_disconnect_finish(client->hc);
            } else {
                harmony_server_write(client->hc, buff->iov_base, buff->iov_len);
            }
        }
    }

    return len;
}

static int audio_pipe_recv_buffers(void *service_pipe,
                            struct iovec *buffers,
                            int num_buffers)
{
    AudioPipe *apipe = (AudioPipe*)service_pipe;
    const struct iovec *buff = buffers;
    const struct iovec *buff_end = buff + num_buffers;
    int len, remain;

    apipe->flags &= ~AUDIO_PIPE_WAKE_READ;

    len = 0;
    for (; buff < buff_end; ++buff) {
        len += buff->iov_len;
    }

    if (QTAILQ_EMPTY(&audio_handler->clients)) {
        return len;
    }

    if (buffer_empty(&audio_handler->input)) {
        return len;
    }

    len = 0;
    remain = audio_handler->input.offset;
    buff = buffers;
    for (; buff < buff_end; ++buff) {
        if (buff->iov_len > remain) {
            memcpy(buff->iov_base, audio_handler->input.buffer, remain);
            buffer_advance(&audio_handler->input, remain);
            len += remain;
            remain = 0;
            break;
        } else {
            memcpy(buff->iov_base, audio_handler->input.buffer, buff->iov_len);
            buffer_advance(&audio_handler->input, buff->iov_len);
            len += buff->iov_len;
            remain -= buff->iov_len;
        }
    }

    return len;
}

static unsigned audio_pipe_poll(void *service_pipe)
{
    AudioPipe *apipe = (AudioPipe*)service_pipe;
    unsigned ret = 0;

    apipe->flags |= AUDIO_PIPE_WAKE_POLL;

    ret |= VIRTIO_PIPE_POLL_OUT;

    if (!buffer_empty(&audio_handler->input))
        ret |= VIRTIO_PIPE_POLL_IN;

    return ret;
}

static void audio_pipe_wake_read(void *service_pipe)
{
    AudioPipe *apipe = (AudioPipe*)service_pipe;

    apipe->flags |= AUDIO_PIPE_WAKE_READ;
}

static void audio_pipe_wake_write(void *service_pipe)
{
    AudioPipe *apipe = (AudioPipe*)service_pipe;

    apipe->flags |= AUDIO_PIPE_WAKE_WRITE;
}

static const HarmonyPipeFuncs audio_pipe_funcs = {
    .create = audio_pipe_create,
    .close = audio_pipe_close,
    .send_buffers = audio_pipe_send_buffers,
    .recv_buffers = audio_pipe_recv_buffers,
    .poll = audio_pipe_poll,
    .wake_read = audio_pipe_wake_read,
    .wake_write = audio_pipe_wake_write,
};

void harmony_pipe_add_service_audio(void)
{
    audio_handler = g_new0(AudioHandler, 1);
    QTAILQ_INIT(&audio_handler->pipes);
    QTAILQ_INIT(&audio_handler->clients);
    buffer_init(&audio_handler->input, "audio-input");

    harmony_pipe_add_service("audio", NULL, &audio_pipe_funcs);

    harmony_server_add_path_handler("/audio", audio_pipe_connect, 
                                audio_pipe_read, 
                                audio_pipe_disconnect, audio_handler);
}
