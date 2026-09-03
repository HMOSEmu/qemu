#ifndef QEMU_HARMONY_PIPE_H
#define QEMU_HARMONY_PIPE_H

#include "hw/virtio/virtio-pipe.h"

typedef struct HarmonyPipeFuncs {
    void* (*create)(const void *pipe, 
                const void *opaque, 
                const struct HarmonyPipeFuncs *funcs,
                const char *args);
    void (*close)(void *service_pipe);
    int (*send_buffers)(void *service_pipe,
                       const struct iovec *buffers,
                       int num_buffers);
    int (*recv_buffers)(void *service_pipe,
                       struct iovec *buffers,
                       int num_buffers);
    unsigned (*poll)(void *service_pipe);
    void (*wake_read)(void *service_pipe);
    void (*wake_write)(void *service_pipe);
} HarmonyPipeFuncs;

typedef struct HarmonyPipe {
    const HarmonyPipeFuncs    *vtbl;
    const VirtIOPipe          *pipe;
} HarmonyPipe;

extern void qemu_harmony_pipe_init(void);

extern void harmony_pipe_add_service(const char *name,
                                   const void *opaque,
                                   const HarmonyPipeFuncs *funcs);

extern void harmony_pipe_host_close(const VirtIOPipe *pipe);
extern void harmony_pipe_host_wake_read(const VirtIOPipe *pipe);
extern void harmony_pipe_host_wake_write(const VirtIOPipe *pipe);

#endif // QEMU_HARMONY_PIPE_H
