#include "qemu/osdep.h"
#include "harmony-pipe.h"
#include "qemu/iov.h"

typedef struct ServiceData {
    QSLIST_ENTRY(ServiceData) entry;

    const char                *name;
    const void                *opaque;
    const HarmonyPipeFuncs    *funcs;
} ServiceData;

static QSLIST_HEAD(, ServiceData) services;

void harmony_pipe_add_service(const char *name,
                            const void *opaque,
                            const HarmonyPipeFuncs *funcs)
{
    ServiceData *service = g_new(ServiceData, 1);
    service->name = name;
    service->opaque = opaque;
    service->funcs = funcs;

    QSLIST_INSERT_HEAD(&services, service, entry);
}

void harmony_pipe_host_wake_read(const VirtIOPipe *pipe)
{
    pipe->vtbl->host_wakeup_read((VirtIOPipe*)pipe);
}

void harmony_pipe_host_wake_write(const VirtIOPipe *pipe)
{
    pipe->vtbl->host_wakeup_write((VirtIOPipe*)pipe);
}

void harmony_pipe_host_close(const VirtIOPipe *pipe)
{
    pipe->vtbl->host_close((VirtIOPipe*)pipe);
}

static ServiceData* harmony_pipe_find_service(const char *name)
{
    ServiceData *service;

    QSLIST_FOREACH(service, &services, entry) {
        if (!strcmp(service->name, name)) {
            return service;
        }
    }

    return NULL;
}

static void* harmony_pipe_create(const char *name, 
                            const void *pipe, 
                            const char *args)
{
    ServiceData *service = harmony_pipe_find_service(name);
    if (service == NULL)
        return NULL;

    return service->funcs->create(pipe, service->opaque, 
                                service->funcs, args);
}

static HostPipe* harmony_pipe_guest_open(VirtIOPipe *pipe)
{
    HarmonyPipe *host_pipe = g_new(HarmonyPipe, 1);
    host_pipe->vtbl = NULL;
    host_pipe->pipe = pipe;
    return (HostPipe*)host_pipe;
}
    
static void harmony_pipe_guest_close(HostPipe *host_pipe)
{
    if (host_pipe == NULL)
        return;

    HarmonyPipe *pipe = (HarmonyPipe *)host_pipe;
    if (pipe->vtbl == NULL) {
        g_free(pipe);
        return;
    }
    
    pipe->vtbl->close(host_pipe);
}

static int harmony_pipe_guest_recv(HostPipe *host_pipe,
                            struct iovec *buffers,
                            int num_buffers)
{
    if (host_pipe == NULL)
        return VIRTIO_PIPE_ERROR_INVAL;

    HarmonyPipe *pipe = (HarmonyPipe *)host_pipe;
    if (pipe->vtbl == NULL)
        return VIRTIO_PIPE_ERROR_INVAL;
    
    return pipe->vtbl->recv_buffers(host_pipe, buffers, num_buffers);
}
    
static int harmony_pipe_guest_send(HostPipe **host_pipe,
                            const struct iovec *buffers,
                            int num_buffers)
{
    if (*host_pipe == NULL)
        return VIRTIO_PIPE_ERROR_INVAL;

    HarmonyPipe *pipe = (HarmonyPipe *)*host_pipe;
    if (pipe->vtbl == NULL) {
        char data[128] = { 0 };
        int len;

        len = iov_to_buf(buffers, num_buffers, 
                        0, data, sizeof(data));
        if ((len < 5) 
            || memcmp(data, "pipe:", 5))
            return 0;

        char *name = data + 5;
        char *args = strchr(name, ':');
        if (args) {
            *args++ = '\0';
        }

        HostPipe *real_pipe = harmony_pipe_create(name, pipe->pipe, args);
        if (real_pipe == NULL)
            return 0;

        *host_pipe = real_pipe;
        g_free(pipe);

        return len;
    } else {
        return pipe->vtbl->send_buffers(*host_pipe, buffers, num_buffers);
    }
}

static void harmony_pipe_guest_wake_read(HostPipe *host_pipe)
{
    if (host_pipe == NULL)
        return;

    HarmonyPipe *pipe = (HarmonyPipe *)host_pipe;
    if (pipe->vtbl == NULL)
        return;

    pipe->vtbl->wake_read(host_pipe);
}

static void harmony_pipe_guest_wake_write(HostPipe *host_pipe)
{
    if (host_pipe == NULL)
        return;

    HarmonyPipe *pipe = (HarmonyPipe *)host_pipe;
    if (pipe->vtbl == NULL)
        return;

    pipe->vtbl->wake_write(host_pipe);
}

static VirtIOPipePollFlags harmony_pipe_guest_poll(HostPipe *host_pipe)
{
    if (host_pipe == NULL)
        return -1;

    HarmonyPipe *pipe = (HarmonyPipe *)host_pipe;
    if (pipe->vtbl == NULL)
        return -1;

    return pipe->vtbl->poll(host_pipe);
}

/*void harmony_pipe_guest_pre_load(QEMUFile *file)
{

}
    
void harmony_pipe_guest_post_load(QEMUFile *file)
{

}

void harmony_pipe_guest_pre_save(QEMUFile *file)
{

}

void harmony_pipe_guest_post_save(QEMUFile *file)
{

}
    
HostPipe* harmony_pipe_guest_load(QEMUFile *file, 
                            VirtIOPipe *pipe,
                            char *force_close)
{

}

void harmony_pipe_guest_save(HostPipe *host_pipe, 
                            QEMUFile *file)
{

}*/

static const VirtIOPipeServiceOps harmony_pipe_ops = {
    .guest_open = harmony_pipe_guest_open,
    .guest_send = harmony_pipe_guest_send,
    .guest_recv = harmony_pipe_guest_recv,
    .guest_wake_read = harmony_pipe_guest_wake_read,
    .guest_wake_write = harmony_pipe_guest_wake_write,
    .guest_poll = harmony_pipe_guest_poll,
    .guest_close = harmony_pipe_guest_close,
};

void qemu_harmony_pipe_init(void)
{
    QSLIST_INIT(&services);

    virtio_pipe_set_service_ops(&harmony_pipe_ops);
}
