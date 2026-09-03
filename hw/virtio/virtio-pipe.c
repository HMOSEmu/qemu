#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/iov.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/qdev-properties.h"
#include "hw/virtio/virtio-pipe.h"
#include "include/qemu/lockable.h"
#include "sysemu/rng.h"
#include "sysemu/runstate.h"
#include "qom/object_interfaces.h"
#include "trace.h"

static HostPipe *null_guest_open(VirtIOPipe *pipe)
{
    (void)pipe;
    return NULL;
}

static int null_guest_send(HostPipe **host_pipe, 
                        const struct iovec *buffers, 
                        int num_buffers) 
{
    int i, length;

    length = 0;
    for (i = 0; i < num_buffers; ++i) {
        struct iovec buffer = buffers[i];
        length += buffer.iov_len;
    }
    
    return length;
}

static int null_guest_recv(HostPipe *host_pipe, 
                        struct iovec *buffers, 
                        int num_buffers) 
{
    int i, length;

    length = 0;
    for (i = 0; i < num_buffers; ++i) {
        struct iovec buffer = buffers[i];
        length += buffer.iov_len;
    }
    
    return length;
}

static void null_guest_close(HostPipe *host_pipe)
{
    (void)host_pipe;
}

static HostPipe *null_guest_load(QEMUFile *file,
                                VirtIOPipe *pipe,
                                char *force_close)
{
    (void)file;
    (void)pipe;
    (void)force_close;
    return NULL;
}

static void null_guest_pre_post_save_load(QEMUFile *file) 
{
    (void)file;
}

static const VirtIOPipeServiceOps s_null_service_ops = {
    .guest_open = null_guest_open,
    .guest_send = null_guest_send,
    .guest_recv = null_guest_recv,
    .guest_close = null_guest_close,
    .guest_load = null_guest_load,
    .guest_pre_load = null_guest_pre_post_save_load,
    .guest_post_load = null_guest_pre_post_save_load,
    .guest_pre_save = null_guest_pre_post_save_load,
    .guest_post_save = null_guest_pre_post_save_load,
};

static const VirtIOPipeServiceOps* service_ops = &s_null_service_ops;

void virtio_pipe_set_service_ops(const VirtIOPipeServiceOps* ops) 
{
    service_ops = ops ? ops : &s_null_service_ops;
}

const VirtIOPipeServiceOps* virtio_pipe_get_service_ops(void) 
{
    return service_ops;
}

static int virtio_pipe_fill_ctrl_imsg(VirtIOPipeDevice *vp_dev, 
                                    uint16_t cmd_id, uint16_t idx)
{
    VirtQueueElement *elem;
    VirtQueue *vq;
    virtio_pipe_control ctrl;

    vq = vp_dev->ctrl_ivq;
    if (!virtio_queue_ready(vq)) {
        return VIRTIO_PIPE_ERROR_INVAL;
    }

    elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
    if (!elem) {
        return VIRTIO_PIPE_ERROR_AGAIN;
    }

    ctrl.cmd_id = cmd_id;
    ctrl.index = idx;
    cpu_to_le16(ctrl.cmd_id);
    cpu_to_le16(ctrl.index);
    iov_from_buf(elem->in_sg, elem->in_num, 0, &ctrl, sizeof(ctrl));

    virtqueue_push(vq, elem, sizeof(ctrl));
    g_free(elem);

    return 0;
}

static void virtio_pipe_drain_ctrl_msgs(VirtIOPipeDevice *vp_dev)
{
    VirtIOPipeCtrlMsg *msg;
    int err;
    unsigned int count = 0;

    WITH_QEMU_LOCK_GUARD(&vp_dev->ctrl_msgq_lock) {
        for (;;) {
            msg = QSIMPLEQ_FIRST(&vp_dev->ctrl_msgq);
            if (msg == NULL) {
                break;
            }

            err = virtio_pipe_fill_ctrl_imsg(vp_dev, msg->cmd_id, msg->index);
            if (err < 0) {
                break;
            }

            ++count;

            QSIMPLEQ_REMOVE_HEAD(&vp_dev->ctrl_msgq, next);
            g_free(msg);
        }
    }

    if (count > 0) {
        virtio_notify(VIRTIO_DEVICE(vp_dev), vp_dev->ctrl_ivq);
    }
}

static void virtio_pipe_ctrl_imsg_cb(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOPipeDevice *vp_dev = VIRTIO_PIPE(vdev);

    virtio_pipe_drain_ctrl_msgs(vp_dev);
}

static int virtio_pipe_push_ctrl_msg(VirtIOPipeDevice *vp_dev, 
                                uint16_t cmd_id, uint16_t idx)
{
    VirtIOPipeCtrlMsg *msg;

    msg = g_new0(VirtIOPipeCtrlMsg, 1);
    if (!msg) {
        return VIRTIO_PIPE_ERROR_NOMEM;
    }

    msg->cmd_id = cmd_id;
    msg->index = idx;

    WITH_QEMU_LOCK_GUARD(&vp_dev->ctrl_msgq_lock) {
        QSIMPLEQ_INSERT_TAIL(&vp_dev->ctrl_msgq, msg, next);
    }

    return 0;
}

static int virtio_pipe_send_ctrl_msg(VirtIOPipe *pipe, 
                                uint16_t cmd_id, uint16_t idx)
{
    if (!pipe) {
        return VIRTIO_PIPE_ERROR_INVAL;
    }

    VirtIOPipeDevice *vp_dev = pipe->dev;
    int err;

    err = virtio_pipe_push_ctrl_msg(vp_dev, cmd_id, idx);
    if (!err) {
        virtio_pipe_drain_ctrl_msgs(vp_dev);
    }

    return err;
}

static void virtio_pipe_host_wakeup_read(VirtIOPipe *pipe)
{
    virtio_pipe_send_ctrl_msg(pipe, VIRTIO_PIPE_CMD_ID_WAKEUP_READ, pipe->id);
}

static void virtio_pipe_host_wakeup_write(VirtIOPipe *pipe)
{
    virtio_pipe_send_ctrl_msg(pipe, VIRTIO_PIPE_CMD_ID_WAKEUP_WRITE, pipe->id);
}

static void virtio_pipe_host_close(VirtIOPipe *pipe)
{
    if (!pipe->closed) {
        pipe->closed = true;
        virtio_pipe_send_ctrl_msg(pipe, VIRTIO_PIPE_CMD_ID_CLOSE, pipe->id);
    }
}

static int virtio_pipe_open(VirtIOPipeDevice *vp_dev, int idx) 
{
    static const VirtIOPipeFuncs vtbl = {
        .host_wakeup_read = virtio_pipe_host_wakeup_read,
        .host_wakeup_write = virtio_pipe_host_wakeup_write,
        .host_close = virtio_pipe_host_close,
    };

    VirtIOPipe *pipe;

    pipe = g_new(VirtIOPipe, 1);
    if (!pipe) {
        return VIRTIO_PIPE_ERROR_NOMEM;
    }

    pipe->vtbl = &vtbl;
    pipe->closed = false;
    pipe->id = idx;
    pipe->dev = vp_dev;
    pipe->vq = vp_dev->pipe_vqs[idx];
    pipe->host_pipe = service_ops->guest_open(pipe);

    vp_dev->pipes[idx] = pipe;

    return VIRTIO_PIPE_OK;
}

static void virtio_pipe_close(VirtIOPipeDevice *vp_dev, int idx)
{
    VirtIOPipe *pipe = vp_dev->pipes[idx];

    virtqueue_drop_all(pipe->vq);

    service_ops->guest_close(pipe->host_pipe);

    vp_dev->pipes[idx] = NULL;
    g_free(pipe);
}

static int virtio_pipe_process_ctrl_omsg(VirtIOPipeDevice *vp_dev, 
                                uint16_t cmd_id, uint16_t index)
{
    VirtIOPipe *pipe;
    int status;

    if (cmd_id == VIRTIO_PIPE_CMD_ID_OPEN) {
        return virtio_pipe_open(vp_dev, index);;
    }

    pipe = vp_dev->pipes[index];
    if ((pipe == NULL) 
        || (pipe->closed && (cmd_id != VIRTIO_PIPE_CMD_ID_CLOSE))) {
        return VIRTIO_PIPE_ERROR_INVAL;
    }

    status = VIRTIO_PIPE_OK;
    switch (cmd_id) {
        case VIRTIO_PIPE_CMD_ID_CLOSE:
            virtio_pipe_close(vp_dev, index);
            break;
        case VIRTIO_PIPE_CMD_ID_WAKE_ON_READ:
            service_ops->guest_wake_read(pipe->host_pipe);
            break;
        case VIRTIO_PIPE_CMD_ID_WAKE_ON_WRITE:
            service_ops->guest_wake_write(pipe->host_pipe);
            break;
        case VIRTIO_PIPE_CMD_ID_POLL:
            status = service_ops->guest_poll(pipe->host_pipe);
            break;
        default:
            break;
    }

    return status;
}

static void virtio_pipe_ctrl_omsg_cb(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOPipeDevice *vp_dev = VIRTIO_PIPE(vdev);
    virtio_pipe_control ctrl;
    VirtQueueElement *elem;
    int len;

    for (;;) {
        elem = virtqueue_pop(vp_dev->ctrl_ovq, sizeof(VirtQueueElement));
        if (!elem) {
            break;
        }

        memset(&ctrl, 0, sizeof(ctrl));

        len = iov_to_buf(elem->in_sg, elem->in_num,
                         0, &ctrl, sizeof(ctrl));

        ctrl.status = virtio_pipe_process_ctrl_omsg(vp_dev, 
                                        le16_to_cpu(ctrl.cmd_id), 
                                        le16_to_cpu(ctrl.index));
        cpu_to_le32s(&ctrl.status);

        len = iov_from_buf(elem->in_sg, elem->in_num,
                           0, &ctrl, sizeof(ctrl));
        
        virtqueue_push(vp_dev->ctrl_ovq, elem, len);
        g_free(elem);
    }
}

static VirtIOPipe *find_pipe_by_virtqueue(VirtIOPipeDevice *vp_dev, VirtQueue *vq)
{
    int i;

    for (i = 0; i < vp_dev->conf.max_pipe_num; ++i) {
        if ((vp_dev->pipes[i] != NULL) 
            && (vp_dev->pipes[i]->vq == vq)) {
            return vp_dev->pipes[i];
        }   
    }

    return NULL;
}

static void virtio_pipe_data_cb(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOPipeDevice *vp_dev = VIRTIO_PIPE(vdev);
    VirtQueueElement *s_elem;
    VirtQueueElement *d_elem;
    int consumed_size = 0;
    int status = 0;
    __virtio32 result;

    VirtIOPipe *pipe = find_pipe_by_virtqueue(vp_dev, vq);
    if ((pipe == NULL) 
        || (pipe->closed))
        return;

    // first element must be status
    s_elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
    if (!s_elem || (s_elem->in_num != 1)) {
        return;
    }

    d_elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
    if (d_elem->out_num > 0) {
        status = service_ops->guest_send(&pipe->host_pipe,
                                        d_elem->out_sg,
                                        d_elem->out_num);
        if (status > 0) {
            consumed_size += status;
        }
    } else if (d_elem->in_num > 0) {
        status = service_ops->guest_recv(pipe->host_pipe,
                                        d_elem->in_sg,
                                        d_elem->in_num);
        if (status > 0) {
            consumed_size += status;
        }
    }

    if (status <= 0) {
        result = status;
    } else {
        result = consumed_size;
    }
    cpu_to_le32s(&result);
    memcpy(s_elem->in_sg[0].iov_base, &result, sizeof(status));

    virtqueue_push(vq, s_elem, sizeof(status));
    virtqueue_push(vq, d_elem, consumed_size);
}

static void virtio_pipe_device_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOPipeDevice *vp_dev = VIRTIO_PIPE(dev);
    int i;

    virtio_init(vdev, VIRTIO_ID_PIPE, sizeof(virtio_pipe_config));

    vp_dev->pipe_vqs = g_new(VirtQueue*, vp_dev->conf.max_pipe_num);
    vp_dev->pipes = g_new(VirtIOPipe*, vp_dev->conf.max_pipe_num);
    if (!vp_dev->pipe_vqs || !vp_dev->pipes) {
        return;
    }
    
    for (i = 0; i < vp_dev->conf.max_pipe_num; ++i) {
        vp_dev->pipe_vqs[i] = virtio_add_queue(vdev, 256, virtio_pipe_data_cb);
        vp_dev->pipes[i] = NULL;
    }

    vp_dev->ctrl_ivq = virtio_add_queue(vdev, 32, virtio_pipe_ctrl_imsg_cb);
    vp_dev->ctrl_ovq = virtio_add_queue(vdev, 32, virtio_pipe_ctrl_omsg_cb);

    qemu_mutex_init(&vp_dev->ctrl_msgq_lock);
    QSIMPLEQ_INIT(&vp_dev->ctrl_msgq);
}

static void virtio_pipe_device_unrealize(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOPipeDevice *vp_dev = VIRTIO_PIPE(dev);
    int i;
    
    virtio_delete_queue(vp_dev->ctrl_ovq);
    virtio_delete_queue(vp_dev->ctrl_ivq);

    for (i = 0; i < vp_dev->conf.max_pipe_num; ++i) {
        if (vp_dev->pipes[i] != NULL) {
            virtio_pipe_close(vp_dev, i);
        }
        virtio_delete_queue(vp_dev->pipe_vqs[i]);
    }

    g_free(vp_dev->pipes);
    g_free(vp_dev->pipe_vqs);

    virtio_cleanup(vdev);
}

static uint64_t virtio_pipe_get_features(VirtIODevice *vdev, 
                                    uint64_t f, Error **errp)
{
    return f;
}

static void virtio_pipe_get_config(VirtIODevice *vdev, uint8_t *config)
{
    VirtIOPipeDevice *vp_dev = VIRTIO_PIPE(vdev);
    virtio_pipe_config *vp_conf = (virtio_pipe_config *)config;

    memcpy(vp_conf, &vp_dev->conf, sizeof(*vp_conf));
    cpu_to_le16s(&vp_conf->max_pipe_num);
}

static Property virtio_pipe_properties[] = {
    DEFINE_PROP_UINT16("max_num", VirtIOPipeDevice, conf.max_pipe_num,
                       MAX_PIPE_NUM_DEFAULT),
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_pipe_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

    device_class_set_props(dc, virtio_pipe_properties);
    //dc->vmsd = &vmstate_virtio_pipe;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    vdc->realize      = virtio_pipe_device_realize;
    vdc->unrealize    = virtio_pipe_device_unrealize;
    vdc->get_features = virtio_pipe_get_features;
    //vdc->set_status   = virtio_pipe_set_status;
    //vdc->reset        = virtio_pipe_reset;
    vdc->get_config   = virtio_pipe_get_config;
}

static const TypeInfo virtio_pipe_info = {
    .name = TYPE_VIRTIO_PIPE,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOPipeDevice),
    .class_init = virtio_pipe_class_init
};

static void virtio_register_types(void)
{
    type_register_static(&virtio_pipe_info);
}

type_init(virtio_register_types)
