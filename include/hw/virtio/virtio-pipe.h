#ifndef QEMU_VIRTIO_PIPE_H
#define QEMU_VIRTIO_PIPE_H

#include "qemu/osdep.h"
#include "standard-headers/linux/virtio_ids.h"
#include "standard-headers/linux/virtio_pipe.h"
#include "hw/virtio/virtio.h"
#include "qom/object.h"

typedef struct virtio_pipe_config virtio_pipe_config;
typedef struct virtio_pipe_control virtio_pipe_control;

typedef struct HostPipe HostPipe;

#define MAX_PIPE_NUM_DEFAULT 32

#define TYPE_VIRTIO_PIPE "virtio-pipe-device"
OBJECT_DECLARE_SIMPLE_TYPE(VirtIOPipeDevice, VIRTIO_PIPE)

struct VirtIOPipe;

typedef struct {
    void (*host_wakeup_read)(struct VirtIOPipe *pipe);
    void (*host_wakeup_write)(struct VirtIOPipe *pipe);
    void (*host_close)(struct VirtIOPipe *pipe);
} VirtIOPipeFuncs;

typedef struct VirtIOPipe {
    const VirtIOPipeFuncs               *vtbl;

    bool                                closed;
    
    uint16_t                            id;
    VirtIOPipeDevice                    *dev;
    VirtQueue                           *vq;

    HostPipe                            *host_pipe;
} VirtIOPipe;

typedef struct VirtIOPipeCtrlMsg {
    QSIMPLEQ_ENTRY(VirtIOPipeCtrlMsg)   next;
    uint16_t                            cmd_id;
    uint16_t                            index;
} VirtIOPipeCtrlMsg;

struct VirtIOPipeDevice {
    VirtIODevice                        parent_obj;
    VirtQueue                           *ctrl_ivq, *ctrl_ovq;

    QemuMutex                           ctrl_msgq_lock;
    QSIMPLEQ_HEAD(, VirtIOPipeCtrlMsg)  ctrl_msgq;

    VirtQueue                           **pipe_vqs;
    VirtIOPipe                          **pipes;

    virtio_pipe_config                  conf;
};

typedef struct {
    HostPipe* (*guest_open)(VirtIOPipe *pipe);
    void (*guest_close)(HostPipe *host_pipe);

    int (*guest_recv)(HostPipe *host_pipe,
                      struct iovec *buffers,
                      int num_buffers);
    int (*guest_send)(HostPipe **host_pipe,
                      const struct iovec *buffers,
                      int num_buffers);

    void (*guest_wake_read)(HostPipe *host_pipe);
    void (*guest_wake_write)(HostPipe *host_pipe);

    VirtIOPipePollFlags (*guest_poll)(HostPipe *host_pipe);

    void (*guest_pre_load)(QEMUFile *file);
    void (*guest_post_load)(QEMUFile *file);
    void (*guest_pre_save)(QEMUFile *file);
    void (*guest_post_save)(QEMUFile *file);
    HostPipe* (*guest_load)(QEMUFile *file, 
                            VirtIOPipe *pipe,
                            char *force_close);
    void (*guest_save)(HostPipe *host_pipe, 
                       QEMUFile *file);
} VirtIOPipeServiceOps;

extern void virtio_pipe_set_service_ops(const VirtIOPipeServiceOps *ops);
extern const VirtIOPipeServiceOps* virtio_pipe_get_service_ops(void);

#endif
