#include "qemu/osdep.h"
#include "qemu/option.h"
#include "qemu/cutils.h"
#include "qemu/config-file.h"
#include "qemu/help_option.h"
#include "qapi/error.h"
#include "harmony-server.h"
#include "harmony-websock.h"
#include "display-handler.h"
#include "input-handler.h"

#define HARMONY_SERVER_MAX_READ_BUFFER 1048576

static HarmonyServer *hs = NULL;

void harmony_server_add_path_handler(const char *path, 
                                    HarmonyServerConnectFunc connect_handler,
                                    HarmonyServerReadFunc read_handler,
                                    HarmonyServerDisconnectFunc disconnect_handler,
                                    void *opaque)
{
    HarmonyServerHandler *handler = g_new0(HarmonyServerHandler, 1);
    handler->path = path;
    handler->connect_handler = connect_handler;
    handler->read_handler = read_handler;
    handler->disconnect_handler = disconnect_handler;
    handler->opaque = opaque;

    QTAILQ_INSERT_TAIL(&hs->handlers, handler, next);
}

void harmony_server_remove_path_handler(const char *path)
{
    int found = 0;
    HarmonyServerHandler *handler;

    QTAILQ_FOREACH(handler, &hs->handlers, next) {
        if (!strcmp(handler->path, path)) {
            found = 1;
            break;
        }
    }

    if (found) {
        QTAILQ_REMOVE(&hs->handlers, handler, next);
        g_free(handler);
    }
}

static HarmonyServerHandler* harmony_server_get_path_handler(const char *path)
{
    int found = 0;
    HarmonyServerHandler *handler;

    QTAILQ_FOREACH(handler, &hs->handlers, next) {
        if (!strcmp(handler->path, path)) {
            found = 1;
            break;
        }
    }

    if (found) {
        return handler;
    } else {
        return NULL;
    }
}

static gboolean harmony_server_check_path(const char *path, void *opaque)
{
    HarmonyClient *hc = opaque;
    int found = 0;
    HarmonyServerHandler *handler;

    QTAILQ_FOREACH(handler, &hc->hs->handlers, next) {
        if (!strcmp(handler->path, path)) {
            found = 1;
            break;
        }
    }

    return found;
}

void harmony_server_disconnect_start(HarmonyClient *hc)
{
    if (hc->disconnecting) {
        return;
    }
    
    if (hc->ioc_tag) {
        g_source_remove(hc->ioc_tag);
        hc->ioc_tag = 0;
    }
    qio_channel_close(hc->ioc, NULL);
    hc->disconnecting = TRUE;
}

void harmony_server_disconnect_finish(HarmonyClient *hc)
{
    hc->disconnect_handler(hc, hc->opaque);

    harmony_server_lock_output(hc);

    buffer_free(&hc->input);
    buffer_free(&hc->output);

    harmony_server_unlock_output(hc);

    QTAILQ_REMOVE(&hc->hs->clients, hc, next);

    object_unref(OBJECT(hc->ioc));
    hc->ioc = NULL;
    object_unref(OBJECT(hc->sioc));
    hc->sioc = NULL;
    hc->magic = 0;
    
    g_free(hc);
}

static size_t harmony_server_client_io_error(HarmonyClient *hc, 
                                ssize_t ret, Error *err)
{
    if (ret <= 0) {
        if (ret == 0) {
            harmony_server_disconnect_start(hc);
        } else if (ret != QIO_CHANNEL_ERR_BLOCK) {
            harmony_server_disconnect_start(hc);
        }

        error_free(err);
        return 0;
    }
    return ret;
}

int harmony_server_write(HarmonyClient *hc, const void *data, size_t len)
{
    assert(hc->magic == HARMONY_SERVER_MAGIC);
    if (hc->disconnecting) {
        return 0;
    }

    buffer_reserve(&hc->output, len);

    if (hc->ioc != NULL && buffer_empty(&hc->output)) {
        if (hc->ioc_tag) {
            g_source_remove(hc->ioc_tag);
        }
        hc->ioc_tag = qio_channel_add_watch(
            hc->ioc, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_OUT,
            harmony_server_client_io, hc, NULL);
    }

    buffer_append(&hc->output, data, len);

    return len;
}

static size_t harmony_server_client_write_buf(HarmonyClient *hc, const uint8_t *data, size_t datalen)
{
    Error *err = NULL;
    ssize_t ret;
    ret = qio_channel_write(hc->ioc, (const char *)data, datalen, &err);

    return harmony_server_client_io_error(hc, ret, err);
}

static int harmony_server_client_write_locked(HarmonyClient *hc)
{
    size_t ret;

    ret = harmony_server_client_write_buf(hc, hc->output.buffer, hc->output.offset);
    if (!ret)
        return 0;

    buffer_advance(&hc->output, ret);

    if (buffer_empty(&hc->output)) {
        if (hc->ioc_tag) {
            g_source_remove(hc->ioc_tag);
        }
        hc->ioc_tag = qio_channel_add_watch(
            hc->ioc, G_IO_IN | G_IO_HUP | G_IO_ERR,
            harmony_server_client_io, hc, NULL);
    }

    return ret;
}

static void harmony_server_client_write(HarmonyClient *hc)
{
    assert(hc->magic == HARMONY_SERVER_MAGIC);

    harmony_server_lock_output(hc);
    if (!buffer_empty(&hc->output)) {
        harmony_server_client_write_locked(hc);
    } else if (hc->ioc != NULL) {
        if (hc->ioc_tag) {
            g_source_remove(hc->ioc_tag);
        }
        hc->ioc_tag = qio_channel_add_watch(
            hc->ioc, G_IO_IN | G_IO_HUP | G_IO_ERR,
            harmony_server_client_io, hc, NULL);
    }
    harmony_server_unlock_output(hc);
}

void harmony_server_flush(HarmonyClient *hc)
{
    harmony_server_lock_output(hc);
    if (hc->ioc != NULL && hc->output.offset) {
        harmony_server_client_write_locked(hc);
    }
    if (hc->disconnecting) {
        if (hc->ioc_tag != 0) {
            g_source_remove(hc->ioc_tag);
        }
        hc->ioc_tag = 0;
    }
    harmony_server_unlock_output(hc);
}

static size_t harmony_server_client_read_buf(HarmonyClient *hc, uint8_t *data, size_t datalen)
{
    ssize_t ret;
    Error *err = NULL;

    ret = qio_channel_read(hc->ioc, (char *)data, datalen, &err);
   
    return harmony_server_client_io_error(hc, ret, err);
}

static size_t harmony_server_client_read_data(HarmonyClient *hc)
{
    size_t ret;
    buffer_reserve(&hc->input, HARMONY_SERVER_MAX_READ_BUFFER);
    ret = harmony_server_client_read_buf(hc, buffer_end(&hc->input), HARMONY_SERVER_MAX_READ_BUFFER);
    if (!ret)
        return 0;
    hc->input.offset += ret;
    return ret;
}

static int harmony_server_client_read(HarmonyClient *hc)
{
    size_t sz = harmony_server_client_read_data(hc);
    if (!sz) {
        if (hc->disconnecting) {
            harmony_server_disconnect_finish(hc);
            return -1;
        }
        return 0;
    }

    if (hc->read_handler) {
        int ret;

        ret = hc->read_handler(hc, hc->opaque, hc->input.buffer, hc->input.offset);
        if (hc->disconnecting) {
            harmony_server_disconnect_finish(hc);
            return -1;
        }

        if (ret > 0) {
            buffer_advance(&hc->input, ret);
        }
    }
    return 0;
}

gboolean harmony_server_client_io(QIOChannel *ioc G_GNUC_UNUSED, 
                                GIOCondition condition, void *opaque)
{
    HarmonyClient *hc = opaque;

    assert(hc->magic == HARMONY_SERVER_MAGIC);

    if (condition & (G_IO_HUP | G_IO_ERR)) {
        harmony_server_disconnect_start(hc);
        return TRUE;
    }

    if (condition & G_IO_IN) {
        if (harmony_server_client_read(hc) < 0) {
            return TRUE;
        }
    }

    if (condition & G_IO_OUT) {
        harmony_server_client_write(hc);
    }

    if (hc->disconnecting) {
        if (hc->ioc_tag != 0) {
            g_source_remove(hc->ioc_tag);
        }
        hc->ioc_tag = 0;
    }
    return TRUE;
}

static void harmony_server_handshake_done(QIOTask *task,
                                    gpointer user_data)
{
    HarmonyClient *hc = user_data;
    Error *err = NULL;
    HarmonyWebsock *hw;

    if (qio_task_propagate_error(task, &err)) {
        goto cleanup;
    } else {
        hw = HARMONY_WEBSOCK(hc->ioc);
        HarmonyServerHandler *handler = harmony_server_get_path_handler(hw->path);
        if (handler != NULL) {
            hc->read_handler = handler->read_handler;
            hc->connect_handler = handler->connect_handler;
            hc->disconnect_handler = handler->disconnect_handler;
            hc->opaque = handler->opaque;

            if (!hc->connect_handler(hc, hc->opaque)) {
                goto cleanup;
            }
        } else {
            goto cleanup;
        }

        if (hc->ioc_tag) {
            g_source_remove(hc->ioc_tag);
        }
        hc->ioc_tag = qio_channel_add_watch(
            hc->ioc, G_IO_IN | G_IO_HUP | G_IO_ERR,
            harmony_server_client_io, hc, NULL);
    }

    return;

cleanup:
    harmony_server_disconnect_start(hc);
    error_free(err);
}

static gboolean harmony_server_handshake_io(QIOChannel *ioc G_GNUC_UNUSED,
                                    GIOCondition condition,
                                    void *opaque)
{
    HarmonyClient *hc = opaque;
    HarmonyWebsock *hw;

    if (hc->ioc_tag) {
        g_source_remove(hc->ioc_tag);
        hc->ioc_tag = 0;
    }

    if (condition & (G_IO_HUP | G_IO_ERR)) {
        harmony_server_disconnect_start(hc);
        return TRUE;
    }

    hw = harmony_websock_new_server(hc->ioc, harmony_server_check_path, hc);
    qio_channel_set_name(QIO_CHANNEL(hw), "harmony-server-websock");

    object_unref(OBJECT(hc->ioc));
    hc->ioc = QIO_CHANNEL(hw);

    harmony_websock_handshake(hw,
                            harmony_server_handshake_done,
                            hc,
                            NULL);

    return TRUE;
}

static void harmony_server_connect(HarmonyServer *hs, 
                                QIOChannelSocket *sioc)
{
    HarmonyClient *hc = g_new0(HarmonyClient, 1);
    hc->magic = HARMONY_SERVER_MAGIC;
    hc->sioc = sioc;
    object_ref(OBJECT(hc->sioc));
    hc->ioc = QIO_CHANNEL(sioc);
    object_ref(OBJECT(hc->ioc));
    hc->hs = hs;
    qemu_mutex_init(&hc->mutex);

    buffer_init(&hc->input, "harmony-server-input/%p", sioc);
    buffer_init(&hc->output, "harmony-server-output/%p", sioc);

    qio_channel_set_blocking(hc->ioc, false, NULL);
    if (hc->ioc_tag) {
        g_source_remove(hc->ioc_tag);
    }

    hc->ioc_tag = qio_channel_add_watch(
                hc->ioc, G_IO_IN | G_IO_HUP | G_IO_ERR,
                harmony_server_handshake_io, hc, NULL);

    QTAILQ_INSERT_TAIL(&hc->hs->clients, hc, next);
}

static void harmony_server_listen_io(QIONetListener *listener,
                                    QIOChannelSocket *cioc,
                                    void *opaque)
{
    HarmonyServer *hs = opaque;

    qio_channel_set_name(QIO_CHANNEL(cioc),
                         "harmony-server-connect");
    qio_channel_set_delay(QIO_CHANNEL(cioc), false);
    harmony_server_connect(hs, cioc);
}

static int harmony_server_listen(HarmonyServer *hs,
                              SocketAddressList *saddr_list,
                              Error **errp)
{
    SocketAddressList *el;

    if (saddr_list) {
        hs->listener = qio_net_listener_new();
        qio_net_listener_set_name(hs->listener, "harmony-server-listen");
        for (el = saddr_list; el; el = el->next) {
            if (qio_net_listener_open_sync(hs->listener,
                                           el->value, 1,
                                           errp) < 0)  {
                return -1;
            }
        }

        qio_net_listener_set_client_func(hs->listener,
                                         harmony_server_listen_io, 
                                         hs, NULL);
    }

    return 0;
}

static void harmony_server_close(HarmonyServer *hs)
{
    if (!hs) {
        return;
    }

    if (hs->listener) {
        qio_net_listener_disconnect(hs->listener);
        object_unref(OBJECT(hs->listener));
    }
    hs->listener = NULL;
}

static int harmony_server_get_address(const char *addrstr,
                                   SocketAddress **retaddr,
                                   Error **errp)
{
    int ret = -1;
    SocketAddress *addr = g_new0(SocketAddress, 1);
    const char *port;
    size_t hostlen;
    uint64_t baseport = 0;
    InetSocketAddress *inet;

    port = strrchr(addrstr, ':');
    if (!port) {
        hostlen = 0;
        port = addrstr;
    } else {
        hostlen = port - addrstr;
        port++;
        if (*port == '\0') {
            error_setg(errp, "harmony server port cannot be empty");
            goto cleanup;
        }
    }

    addr->type = SOCKET_ADDRESS_TYPE_INET;
    inet = &addr->u.inet;
    inet->host = g_strndup(addrstr, hostlen);
    
    if (parse_uint_full(port, 10, &baseport) < 0) {
        error_setg(errp, "can't convert to a number: %s", port);
        goto cleanup;
    }
    if (baseport > 65535) {
        error_setg(errp, "port %s out of range", port);
        goto cleanup;
    }
    inet->port = g_strdup_printf("%d", (int)baseport);

    inet->has_ipv4 = false;
    inet->has_ipv6 = false;

    ret = baseport;
    *retaddr = addr;

 cleanup:
    if (ret < 0) {
        qapi_free_SocketAddress(addr);
    }
    return ret;
}

static int harmony_server_get_addresses(QemuOpts *opts,
                                     SocketAddressList **saddr_list_ret,
                                     Error **errp)
{
    SocketAddress *saddr = NULL;
    g_autoptr(SocketAddressList) saddr_list = NULL;
    SocketAddressList **saddr_tail = &saddr_list;
    QemuOptsIter addriter;
    const char *addr;

    addr = qemu_opt_get(opts, "harmony-server");
    if (addr == NULL || g_str_equal(addr, "none")) {
        return 0;
    }

    qemu_opt_iter_init(&addriter, opts, "harmony-server");
    while ((addr = qemu_opt_iter_next(&addriter)) != NULL) {
        int rv;
        rv = harmony_server_get_address(addr, &saddr, errp);
        if (rv < 0) {
            return -1;
        }
        
        QAPI_LIST_APPEND(saddr_tail, saddr);
    }

    *saddr_list_ret = g_steal_pointer(&saddr_list);
    
    return 0;
}

static void harmony_server_open(QemuOpts *opts, HarmonyServer *hs, Error **errp)
{
    g_autoptr(SocketAddressList) saddr_list = NULL;

    if (!hs) {
        return;
    }
    harmony_server_close(hs);

    if (!opts) {
        return;
    }

    if (harmony_server_get_addresses(opts, &saddr_list, errp) < 0) {
        goto fail;
    }

    if (harmony_server_listen(hs, saddr_list, errp) < 0) {
        goto fail;
    }

    return;

fail:
    harmony_server_close(hs);
}

int harmony_server_init(void *opaque, QemuOpts *opts, Error **errp)
{
    Error *local_err = NULL;

    if (hs != NULL) {
        return -1;
    }

    hs = g_malloc0(sizeof(*hs));

    QTAILQ_INIT(&hs->clients);
    QTAILQ_INIT(&hs->handlers);

    harmony_server_open(opts, hs, &local_err);
    if (local_err != NULL) {
        error_propagate(errp, local_err);
        return -1;
    }

    HarmonyServerDisplay *hsd = harmony_server_display_init(opts);
    harmony_server_input_init(hsd, errp);

    return 0;
}

static QemuOptsList qemu_harmony_server_opts = {
    .name = "harmony-server",
    .head = QTAILQ_HEAD_INITIALIZER(qemu_harmony_server_opts.head),
    .implied_opt_name = "harmony-server",
    .desc = {
        {
            .name = "harmony-server",
            .type = QEMU_OPT_STRING,
        },{
            .name = "quality",
            .type = QEMU_OPT_NUMBER,
        },{
            .name = "refresh_interval",
            .type = QEMU_OPT_NUMBER,
        },
        { /* end of list */ }
    },
};

void harmony_server_opts_parse(const char *str)
{
    QemuOptsList *olist = qemu_find_opts("harmony-server");
    QemuOpts *opts = qemu_opts_parse_noisily(olist, str, !is_help_option(str));

    if (!opts) {
        exit(1);
    }
}

static void harmony_server_register_config(void)
{
    qemu_add_opts(&qemu_harmony_server_opts);
}
opts_init(harmony_server_register_config);
