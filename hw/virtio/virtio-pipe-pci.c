#include "qemu/osdep.h"
#include "hw/virtio/virtio-pci.h"
#include "hw/core/qdev-properties.h"
#include "hw/virtio/virtio-pipe.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_VIRTIO_PIPE_PCI "virtio-pipe-pci-base"
OBJECT_DECLARE_SIMPLE_TYPE(VirtIOPipePCI, VIRTIO_PIPE_PCI)

struct VirtIOPipePCI {
    VirtIOPCIProxy   parent_obj;
    VirtIOPipeDevice vdev;
};

static const Property virtio_pipe_pci_properties[] = {
    DEFINE_PROP_UINT32("vectors", VirtIOPCIProxy, nvectors, 2),
};

static void virtio_pipe_pci_realize(VirtIOPCIProxy *vpci_dev, Error **errp)
{
    VirtIOPipePCI *vp_dev = VIRTIO_PIPE_PCI(vpci_dev);
    DeviceState *vdev = DEVICE(&vp_dev->vdev);

    virtio_pci_force_virtio_1(vpci_dev);
    qdev_realize(vdev, BUS(&vpci_dev->bus), errp);
}

static void virtio_pipe_pci_instance_init(Object *obj)
{
    VirtIOPipePCI *dev = VIRTIO_PIPE_PCI(obj);

    virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                                TYPE_VIRTIO_PIPE);
}

static void virtio_pipe_pci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioPCIClass *k = VIRTIO_PCI_CLASS(klass);
    PCIDeviceClass *pcidev_k = PCI_DEVICE_CLASS(klass);

    device_class_set_props(dc, virtio_pipe_pci_properties);
    k->realize = virtio_pipe_pci_realize;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    pcidev_k->class_id = PCI_CLASS_COMMUNICATION_OTHER;
}

static const VirtioPCIDeviceTypeInfo virtio_pipe_pci_info = {
    .base_name             = TYPE_VIRTIO_PIPE_PCI,
    .generic_name          = "virtio-pipe-pci",
    .transitional_name     = "virtio-pipe-pci-transitional",
    .non_transitional_name = "virtio-pipe-pci-non-transitional",
    .instance_size         = sizeof(VirtIOPipePCI),
    .instance_init         = virtio_pipe_pci_instance_init,
    .class_init            = virtio_pipe_pci_class_init,
};

static void virtio_pci_pipe_register(void)
{
    virtio_pci_types_register(&virtio_pipe_pci_info);
}

type_init(virtio_pci_pipe_register)
