#ifndef HARMONY_SERVER_H
#define HARMONY_SERVER_H

#include "qemu/queue.h"
#include "qemu/buffer.h"
#include "io/net-listener.h"
#include "io/channel-socket.h"

typedef struct HarmonyServerHandler HarmonyServerHandler;
typedef struct HarmonyServer HarmonyServer;
typedef struct HarmonyClient HarmonyClient;

typedef gboolean (*HarmonyServerConnectFunc)(HarmonyClient *hc, void *opaque);
typedef int (*HarmonyServerReadFunc)(HarmonyClient *hc, void *opaque, uint8_t *data, size_t len);
typedef void (*HarmonyServerDisconnectFunc)(HarmonyClient *hc, void *opaque);

struct HarmonyServerHandler {
    const char *path;
    void *opaque;
    HarmonyServerConnectFunc connect_handler;
    HarmonyServerReadFunc read_handler;
    HarmonyServerDisconnectFunc disconnect_handler;

    QTAILQ_ENTRY(HarmonyServerHandler) next;
};

struct HarmonyServer {
    QTAILQ_HEAD(, HarmonyClient) clients;

    QTAILQ_HEAD(, HarmonyServerHandler) handlers;

    QIONetListener *listener;
};

#define HARMONY_SERVER_MAGIC ((uint64_t)0x0835ca890ed1876f)

struct HarmonyClient {
    uint64_t magic;
    QIOChannelSocket *sioc;
    QIOChannel *ioc;
    guint ioc_tag;
    gboolean disconnecting;

    HarmonyServer *hs;

    HarmonyServerConnectFunc connect_handler;
    HarmonyServerReadFunc read_handler;
    HarmonyServerDisconnectFunc disconnect_handler;
    void *opaque;

    QemuMutex mutex;
    Buffer output;
    Buffer input;

    QTAILQ_ENTRY(HarmonyClient) next;
};

extern void harmony_server_opts_parse(const char *str);
extern int harmony_server_init(void *opaque, QemuOpts *opts, Error **errp);
extern void harmony_server_add_path_handler(const char *path, 
                                    HarmonyServerConnectFunc connect_handler,
                                    HarmonyServerReadFunc read_handler,
                                    HarmonyServerDisconnectFunc disconnect_handler,
                                    void *opaque);
extern void harmony_server_remove_path_handler(const char *path);
extern int harmony_server_write(HarmonyClient *hc, const void *data, size_t len);
extern gboolean harmony_server_client_io(QIOChannel *ioc G_GNUC_UNUSED, 
                                GIOCondition condition, void *opaque);
extern void harmony_server_disconnect_start(HarmonyClient *hc);
extern void harmony_server_disconnect_finish(HarmonyClient *hc);
extern void harmony_server_flush(HarmonyClient *hc);

static inline void harmony_server_lock_output(HarmonyClient *hc)
{
    qemu_mutex_lock(&hc->mutex);
}

static inline void harmony_server_unlock_output(HarmonyClient *hc)
{
    qemu_mutex_unlock(&hc->mutex);
}

#endif // HARMONY_SERVER_H
