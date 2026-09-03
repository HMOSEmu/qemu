#include "qemu/osdep.h"
#include "harmony-pipe.h"
#include "loop-pipe.h"
#include "qemu/iov.h"

#define LOOP_SIZE 1024

#define LOOP_PIPE_WAKE_READ  1
#define LOOP_PIPE_WAKE_WRITE 2

typedef struct {
    const HarmonyPipeFuncs  *vtbl;
    const VirtIOPipe        *pipe;

    char                    *buffer;
    size_t                  size;
    size_t                  count;
    size_t                  pos;
    unsigned                flags;
} LoopPipe;

static void* loop_pipe_create(const void *pipe, 
                        const void *opaque, 
                        const HarmonyPipeFuncs *funcs,
                        const char *args)
{
    LoopPipe *lpipe = g_new(LoopPipe, 1);

    lpipe->vtbl = funcs;
    lpipe->pipe = pipe;
    lpipe->buffer = malloc(LOOP_SIZE);
    lpipe->size = LOOP_SIZE;
    lpipe->count = 0;
    lpipe->pos = 0;
    lpipe->flags = 0;
    
    return lpipe;
}

static void loop_pipe_close(void *service_pipe)
{
    LoopPipe *pipe = (LoopPipe*)service_pipe;

    free(pipe->buffer);
    g_free(pipe);
}

static int loop_pipe_send_buffers(void *service_pipe,
                            const struct iovec *buffers,
                            int num_buffers)
{
    LoopPipe *pipe = (LoopPipe*)service_pipe;
    int  ret = 0;
    int  count;
    const struct iovec *buff = buffers;
    const struct iovec *buff_end = buff + num_buffers;

    pipe->flags &= ~LOOP_PIPE_WAKE_WRITE;

    count = 0;
    for (; buff < buff_end; ++buff)
        count += buff->iov_len;

    while (count > pipe->size - pipe->count) {
        size_t new_size = pipe->size * 2;
        char *new_buff = realloc(pipe->buffer, new_size);
        int wpos = pipe->pos + pipe->count;
        if (new_buff == NULL) {
            break;
        }
        if (wpos > pipe->size) {
            wpos -= pipe->size;
            memcpy(new_buff + pipe->size, new_buff, wpos);
        }
        pipe->buffer = new_buff;
        pipe->size = new_size;
    }

    for (buff = buffers; buff < buff_end; ++buff) {
        int avail = pipe->size - pipe->count;
        if (avail <= 0) {
            if (ret == 0)
                ret = VIRTIO_PIPE_ERROR_AGAIN;
            break;
        }
        if (avail > buff->iov_len) {
            avail = buff->iov_len;
        }

        int wpos = pipe->pos + pipe->count;
        if (wpos >= pipe->size) {
            wpos -= pipe->size;
        }
        if (wpos + avail <= pipe->size) {
            memcpy(pipe->buffer + wpos, buff->iov_base, avail);
        } else {
            int  avail2 = pipe->size - wpos;
            memcpy(pipe->buffer + wpos, buff->iov_base, avail2);
            memcpy(pipe->buffer, buff->iov_base + avail2, avail - avail2);
        }
        pipe->count += avail;
        ret += avail;
    }

    if ((pipe->count > 0) 
        && (pipe->flags & LOOP_PIPE_WAKE_READ)) {
        harmony_pipe_host_wake_read(pipe->pipe);
    }

    return ret;
}

static int loop_pipe_recv_buffers(void *service_pipe,
                            struct iovec *buffers,
                            int num_buffers)
{
    LoopPipe *pipe = (LoopPipe*)service_pipe;
    int ret = 0;

    pipe->flags &= ~LOOP_PIPE_WAKE_READ;

    while (num_buffers > 0) {
        int avail = pipe->count;
        if (avail <= 0) {
            if (ret == 0)
                ret = VIRTIO_PIPE_ERROR_AGAIN;
            break;
        }
        if (avail > buffers[0].iov_len) {
            avail = buffers[0].iov_len;
        }

        int rpos = pipe->pos;

        if (rpos + avail <= pipe->size) {
            memcpy(buffers[0].iov_base, pipe->buffer + rpos, avail);
        } else {
            int  avail2 = pipe->size - rpos;
            memcpy(buffers[0].iov_base, pipe->buffer + rpos, avail2);
            memcpy(buffers[0].iov_base + avail2, pipe->buffer, avail - avail2);
        }
        pipe->count -= avail;
        pipe->pos += avail;
        if (pipe->pos >= pipe->size) {
            pipe->pos -= pipe->size;
        }
        ret += avail;
        num_buffers--;
        buffers++;
    }

    if ((pipe->count < pipe->size) 
        && (pipe->flags & LOOP_PIPE_WAKE_WRITE)) {
        harmony_pipe_host_wake_write(pipe->pipe);
    }

    return ret;
}

static unsigned loop_pipe_poll(void *service_pipe)
{
    LoopPipe *pipe = (LoopPipe*)service_pipe;
    unsigned ret = 0;

    ret |= VIRTIO_PIPE_POLL_OUT;

    if (pipe->count > 0)
        ret |= VIRTIO_PIPE_POLL_IN;

    return ret;
}

static void loop_pipe_wake_read(void *service_pipe)
{
    LoopPipe *pipe = (LoopPipe*)service_pipe;

    pipe->flags |= LOOP_PIPE_WAKE_READ;
}

static void loop_pipe_wake_write(void *service_pipe)
{
    LoopPipe *pipe = (LoopPipe*)service_pipe;

    pipe->flags |= LOOP_PIPE_WAKE_WRITE;
}

static const HarmonyPipeFuncs loop_pipe_funcs = {
    .create = loop_pipe_create,
    .close = loop_pipe_close,
    .send_buffers = loop_pipe_send_buffers,
    .recv_buffers = loop_pipe_recv_buffers,
    .poll = loop_pipe_poll,
    .wake_read = loop_pipe_wake_read,
    .wake_write = loop_pipe_wake_write,
};

void harmony_pipe_add_service_loop(void)
{
    harmony_pipe_add_service("loop", NULL, &loop_pipe_funcs);
}
