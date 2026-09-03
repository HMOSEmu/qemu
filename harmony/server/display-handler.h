#ifndef DISPLAY_HANDLER_H
#define DISPLAY_HANDLER_H

#include "qemu/queue.h"
#include "qemu/thread.h"
#include "qemu/bitmap.h"
#include "qemu/buffer.h"
#include "ui/console.h"
#include "harmony-server.h"

typedef struct HarmonyServerDisplayJob HarmonyServerDisplayJob;
typedef struct HarmonyServerDisplayJobQueue HarmonyServerDisplayJobQueue;
typedef struct HarmonyServerDisplay HarmonyServerDisplay;
typedef struct HarmonyClientDisplay HarmonyClientDisplay;

struct HarmonyServerDisplayJob
{
    HarmonyClientDisplay *hcd;
    int seq;

    QTAILQ_ENTRY(HarmonyServerDisplayJob) next;
};

struct HarmonyServerDisplayJobQueue {
    QemuCond cond;
    QemuMutex mutex;
    QemuThread thread;
    bool exit;
    QTAILQ_HEAD(, HarmonyServerDisplayJob) jobs;
};

struct HarmonyServerDisplay {
    QTAILQ_HEAD(, HarmonyClientDisplay) clients;
    DisplayChangeListener dcl;

    DisplaySurface *ds;

    QemuMutex mutex;
    unsigned long fb_seq;
    pixman_image_t *server_fb;
    pixman_image_t *guest_fb;
    pixman_format_code_t guest_format;
    int width;
    int height;
    int quality;
    uint64_t refresh_interval;

    unsigned long jpeg_seq;
    Buffer jpeg;

    int has_dirty;

    HarmonyServerDisplayJobQueue *queue;
};

struct HarmonyClientDisplay {
    HarmonyServerDisplay *hsd;
    HarmonyClient *hc;
    QTAILQ_ENTRY(HarmonyClientDisplay) next;

    Buffer jpeg;

    QEMUBH *bh;
};

#define HARMONY_SERVER_DISPLAY_FB_FORMAT PIXMAN_FORMAT(32, PIXMAN_TYPE_ARGB, 0, 8, 8, 8)
#define HARMONY_SERVER_DISPLAY_FB_BITS   (PIXMAN_FORMAT_BPP(VNC_SERVER_FB_FORMAT))
#define HARMONY_SERVER_DISPLAY_FB_BYTES  ((HARMONY_SERVER_DISPLAY_FB_BITS + 7) / 8)

extern HarmonyServerDisplay* harmony_server_display_init(QemuOpts *opts);

static inline int harmony_server_trylock_display(HarmonyServerDisplay *hsd)
{
    return qemu_mutex_trylock(&hsd->mutex);
}

static inline void harmony_server_lock_display(HarmonyServerDisplay *hsd)
{
    qemu_mutex_lock(&hsd->mutex);
}

static inline void harmony_server_unlock_display(HarmonyServerDisplay *hsd)
{
    qemu_mutex_unlock(&hsd->mutex);
}

#endif // DISPLAY_HANDLER_H
