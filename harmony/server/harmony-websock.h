#ifndef HARMONY_WEBSOCK_H
#define HARMONY_WEBSOCK_H

#include "io/channel.h"
#include "qemu/buffer.h"
#include "io/task.h"
#include "qom/object.h"

#define TYPE_HARMONY_WEBSOCK "harmony-websock"
OBJECT_DECLARE_SIMPLE_TYPE(HarmonyWebsock, HARMONY_WEBSOCK)

typedef union HarmonyWebsockMask HarmonyWebsockMask;

typedef gboolean (*HarmonyWebsockCheckPathFunc)(const char *path, void *opaque);

union HarmonyWebsockMask {
    char c[4];
    uint32_t u;
};

struct HarmonyWebsock {
    QIOChannel parent;
    QIOChannel *master;
    Buffer encinput;
    Buffer encoutput;
    Buffer rawinput;
    size_t payload_remain;
    size_t pong_remain;
    HarmonyWebsockMask mask;
    guint io_tag;
    Error *io_err;
    gboolean io_eof;
    uint8_t opcode;

    HarmonyWebsockCheckPathFunc path_checker;
    char *path;
    void *opaque;
};

HarmonyWebsock* harmony_websock_new_server(QIOChannel *master, 
                                    HarmonyWebsockCheckPathFunc path_checker, 
                                    void *opaque);

void harmony_websock_handshake(HarmonyWebsock *hw,
                            QIOTaskFunc func,
                            gpointer opaque,
                            GDestroyNotify destroy);

#endif // HARMONY_WEBSOCK_H
