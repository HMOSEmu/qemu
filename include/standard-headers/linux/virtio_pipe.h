#ifndef _LINUX_VIRTIO_PIPE_H
#define _LINUX_VIRTIO_PIPE_H

#include "qemu/osdep.h"
#include "standard-headers/linux/virtio_types.h"

#define VIRTIO_PIPE_CMD_ID_OPEN                     0
#define VIRTIO_PIPE_CMD_ID_CLOSE                    1
#define VIRTIO_PIPE_CMD_ID_WAKE_ON_READ             2
#define VIRTIO_PIPE_CMD_ID_WAKE_ON_WRITE            3
#define VIRTIO_PIPE_CMD_ID_POLL                     4
#define VIRTIO_PIPE_CMD_ID_WAKEUP_READ              5
#define VIRTIO_PIPE_CMD_ID_WAKEUP_WRITE             6

struct virtio_pipe_config {
    __virtio16 max_pipe_num;
};

struct virtio_pipe_control {
    __virtio16 cmd_id;
    __virtio16 index;
    __virtio32 status;
};

typedef enum {
    VIRTIO_PIPE_OK          = 0,
    VIRTIO_PIPE_ERROR_INVAL = -1,
    VIRTIO_PIPE_ERROR_AGAIN = -2,
    VIRTIO_PIPE_ERROR_NOMEM = -3,
    VIRTIO_PIPE_ERROR_IO    = -4
} VirtIOPipeError;

typedef enum {
    VIRTIO_PIPE_POLL_IN  = 1 << 0,
    VIRTIO_PIPE_POLL_OUT = 1 << 1,
    VIRTIO_PIPE_POLL_HUP = 1 << 2
} VirtIOPipePollFlags;

#endif /* _LINUX_VIRTIO_PIPE_H */
