#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "harmony-websock.h"
#include "crypto/hash.h"
#include "qemu/iov.h"
#include "qemu/module.h"

/* A 1080x2340 display at JPEG quality 50 can exceed one MiB.  The old
 * limit made qio_channel_writev() emit a complete FIN frame for only the
 * first MiB, so display clients received a truncated JPEG. */
#define HARMONY_WEBSOCK_MAX_BUFFER (16 * 1024 * 1024)

#define HARMONY_WEBSOCK_CLIENT_KEY_LEN 24
#define HARMONY_WEBSOCK_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define HARMONY_WEBSOCK_GUID_LEN (sizeof(HARMONY_WEBSOCK_GUID) - 1)

#define HARMONY_WEBSOCK_HEADER_PROTOCOL "sec-websocket-protocol"
#define HARMONY_WEBSOCK_HEADER_VERSION "sec-websocket-version"
#define HARMONY_WEBSOCK_HEADER_KEY "sec-websocket-key"
#define HARMONY_WEBSOCK_HEADER_UPGRADE "upgrade"
#define HARMONY_WEBSOCK_HEADER_HOST "host"
#define HARMONY_WEBSOCK_HEADER_CONNECTION "connection"

#define HARMONY_WEBSOCK_PROTOCOL_BINARY "binary"
#define HARMONY_WEBSOCK_CONNECTION_UPGRADE "Upgrade"
#define HARMONY_WEBSOCK_UPGRADE_WEBSOCKET "websocket"

#define HARMONY_WEBSOCK_HANDSHAKE_RES_COMMON \
    "Server: Harmony WebSocket\r\n"          \
    "Date: %s\r\n"

#define HARMONY_WEBSOCK_HANDSHAKE_WITH_PROTO_RES_OK     \
    "HTTP/1.1 101 Switching Protocols\r\n"              \
    HARMONY_WEBSOCK_HANDSHAKE_RES_COMMON                \
    "Upgrade: websocket\r\n"                            \
    "Connection: Upgrade\r\n"                           \
    "Sec-WebSocket-Accept: %s\r\n"                      \
    "Sec-WebSocket-Protocol: binary\r\n"                \
    "\r\n"
#define HARMONY_WEBSOCK_HANDSHAKE_RES_OK        \
    "HTTP/1.1 101 Switching Protocols\r\n"      \
    HARMONY_WEBSOCK_HANDSHAKE_RES_COMMON        \
    "Upgrade: websocket\r\n"                    \
    "Connection: Upgrade\r\n"                   \
    "Sec-WebSocket-Accept: %s\r\n"              \
    "\r\n"
#define HARMONY_WEBSOCK_HANDSHAKE_RES_NOT_FOUND     \
    "HTTP/1.1 404 Not Found\r\n"                    \
    HARMONY_WEBSOCK_HANDSHAKE_RES_COMMON            \
    "Connection: close\r\n"                         \
    "\r\n"
#define HARMONY_WEBSOCK_HANDSHAKE_RES_BAD_REQUEST     \
    "HTTP/1.1 400 Bad Request\r\n"                    \
    HARMONY_WEBSOCK_HANDSHAKE_RES_COMMON              \
    "Connection: close\r\n"                           \
    "Sec-WebSocket-Version: "                         \
    HARMONY_WEBSOCK_SUPPORTED_VERSION                 \
    "\r\n"
#define HARMONY_WEBSOCK_HANDSHAKE_RES_SERVER_ERR     \
    "HTTP/1.1 500 Internal Server Error\r\n"         \
    HARMONY_WEBSOCK_HANDSHAKE_RES_COMMON             \
    "Connection: close\r\n"                          \
    "\r\n"
#define HARMONY_WEBSOCK_HANDSHAKE_RES_TOO_LARGE      \
    "HTTP/1.1 403 Request Entity Too Large\r\n"      \
    HARMONY_WEBSOCK_HANDSHAKE_RES_COMMON             \
    "Connection: close\r\n"                          \
    "\r\n"
#define HARMONY_WEBSOCK_HANDSHAKE_DELIM "\r\n"
#define HARMONY_WEBSOCK_HANDSHAKE_END "\r\n\r\n"
#define HARMONY_WEBSOCK_SUPPORTED_VERSION "13"
#define HARMONY_WEBSOCK_HTTP_METHOD "GET"
#define HARMONY_WEBSOCK_HTTP_VERSION "HTTP/1.1"

/* The websockets packet header is variable length
 * depending on the size of the payload... */

/* ...length when using 7-bit payload length */
#define HARMONY_WEBSOCK_HEADER_LEN_7_BIT 6
/* ...length when using 16-bit payload length */
#define HARMONY_WEBSOCK_HEADER_LEN_16_BIT 8
/* ...length when using 64-bit payload length */
#define HARMONY_WEBSOCK_HEADER_LEN_64_BIT 14

/* Length of the optional data mask field in header */
#define HARMONY_WEBSOCK_HEADER_LEN_MASK 4

/* Maximum length that can fit in 7-bit payload size */
#define HARMONY_WEBSOCK_PAYLOAD_LEN_THRESHOLD_7_BIT 126
/* Maximum length that can fit in 16-bit payload size */
#define HARMONY_WEBSOCK_PAYLOAD_LEN_THRESHOLD_16_BIT 65536

/* Magic 7-bit length to indicate use of 16-bit payload length */
#define HARMONY_WEBSOCK_PAYLOAD_LEN_MAGIC_16_BIT 126
/* Magic 7-bit length to indicate use of 64-bit payload length */
#define HARMONY_WEBSOCK_PAYLOAD_LEN_MAGIC_64_BIT 127

/* Bitmasks for accessing header fields */
#define HARMONY_WEBSOCK_HEADER_FIELD_FIN 0x80
#define HARMONY_WEBSOCK_HEADER_FIELD_OPCODE 0x0f
#define HARMONY_WEBSOCK_HEADER_FIELD_HAS_MASK 0x80
#define HARMONY_WEBSOCK_HEADER_FIELD_PAYLOAD_LEN 0x7f
#define HARMONY_WEBSOCK_CONTROL_OPCODE_MASK 0x8

typedef struct HarmonyWebsockHeader HarmonyWebsockHeader;

struct QEMU_PACKED HarmonyWebsockHeader {
    unsigned char b0;
    unsigned char b1;
    union {
        struct QEMU_PACKED {
            uint16_t l16;
            HarmonyWebsockMask m16;
        } s16;
        struct QEMU_PACKED {
            uint64_t l64;
            HarmonyWebsockMask m64;
        } s64;
        HarmonyWebsockMask m;
    } u;
};

typedef struct HarmonyWebsockHTTPHeader HarmonyWebsockHTTPHeader;

struct HarmonyWebsockHTTPHeader {
    char *name;
    char *value;
};

enum {
    HARMONY_WEBSOCK_OPCODE_CONTINUATION = 0x0,
    HARMONY_WEBSOCK_OPCODE_TEXT_FRAME = 0x1,
    HARMONY_WEBSOCK_OPCODE_BINARY_FRAME = 0x2,
    HARMONY_WEBSOCK_OPCODE_CLOSE = 0x8,
    HARMONY_WEBSOCK_OPCODE_PING = 0x9,
    HARMONY_WEBSOCK_OPCODE_PONG = 0xA
};

static void G_GNUC_PRINTF(2, 3)
harmony_websock_handshake_send_res(HarmonyWebsock *hw,
                                const char *resmsg,
                                ...)
{
    va_list vargs;
    char *response;
    size_t responselen;

    va_start(vargs, resmsg);
    response = g_strdup_vprintf(resmsg, vargs);
    responselen = strlen(response);
    buffer_reserve(&hw->encoutput, responselen);
    buffer_append(&hw->encoutput, response, responselen);
    g_free(response);
    va_end(vargs);
}

static gchar* harmony_websock_date_str(void)
{
    g_autoptr(GDateTime) now = g_date_time_new_now_utc();

    return g_date_time_format(now, "%a, %d %b %Y %H:%M:%S GMT");
}

static void harmony_websock_handshake_send_res_err(HarmonyWebsock *hw,
                                                const char *resdata)
{
    char *date = harmony_websock_date_str();
    harmony_websock_handshake_send_res(hw, resdata, date);
    g_free(date);
}

enum {
    HARMONY_WEBSOCK_STATUS_NORMAL = 1000,
    HARMONY_WEBSOCK_STATUS_PROTOCOL_ERR = 1002,
    HARMONY_WEBSOCK_STATUS_INVALID_DATA = 1003,
    HARMONY_WEBSOCK_STATUS_POLICY = 1008,
    HARMONY_WEBSOCK_STATUS_TOO_LARGE = 1009,
    HARMONY_WEBSOCK_STATUS_SERVER_ERR = 1011,
};

static size_t
harmony_websock_extract_headers(HarmonyWebsock *hw,
                                char *buffer,
                                HarmonyWebsockHTTPHeader *hdrs,
                                size_t nhdrsalloc,
                                Error **errp)
{
    char *nl, *sep, *tmp;
    size_t nhdrs = 0;

    nl = strstr(buffer, HARMONY_WEBSOCK_HANDSHAKE_DELIM);
    if (!nl) {
        error_setg(errp, "Missing HTTP header delimiter");
        goto bad_request;
    }
    *nl = '\0';

    tmp = strchr(buffer, ' ');
    if (!tmp) {
        error_setg(errp, "Missing HTTP path delimiter");
        return 0;
    }
    *tmp = '\0';

    if (!g_str_equal(buffer, HARMONY_WEBSOCK_HTTP_METHOD)) {
        error_setg(errp, "Unsupported HTTP method %s", buffer);
        goto bad_request;
    }

    buffer = tmp + 1;
    tmp = strchr(buffer, ' ');
    if (!tmp) {
        error_setg(errp, "Missing HTTP version delimiter");
        goto bad_request;
    }
    *tmp = '\0';

    if (hw->path_checker(buffer, hw->opaque)) {
        hw->path = strdup(buffer);
    } else {
        harmony_websock_handshake_send_res_err(
            hw, HARMONY_WEBSOCK_HANDSHAKE_RES_NOT_FOUND);
        error_setg(errp, "Unexpected HTTP path %s", buffer);
        return 0;
    }

    buffer = tmp + 1;

    if (!g_str_equal(buffer, HARMONY_WEBSOCK_HTTP_VERSION)) {
        error_setg(errp, "Unsupported HTTP version %s", buffer);
        goto bad_request;
    }

    buffer = nl + strlen(HARMONY_WEBSOCK_HANDSHAKE_DELIM);

    do {
        HarmonyWebsockHTTPHeader *hdr;

        nl = strstr(buffer, HARMONY_WEBSOCK_HANDSHAKE_DELIM);
        if (nl) {
            *nl = '\0';
        }

        sep = strchr(buffer, ':');
        if (!sep) {
            error_setg(errp, "Malformed HTTP header");
            goto bad_request;
        }
        *sep = '\0';
        sep++;
        while (*sep == ' ') {
            sep++;
        }

        if (nhdrs >= nhdrsalloc) {
            error_setg(errp, "Too many HTTP headers");
            goto bad_request;
        }

        hdr = &hdrs[nhdrs++];
        hdr->name = buffer;
        hdr->value = sep;

        for (tmp = hdr->name; *tmp; tmp++) {
            *tmp = g_ascii_tolower(*tmp);
        }

        if (nl) {
            buffer = nl + strlen(HARMONY_WEBSOCK_HANDSHAKE_DELIM);
        }
    } while (nl != NULL);

    return nhdrs;

 bad_request:
    harmony_websock_handshake_send_res_err(
        hw, HARMONY_WEBSOCK_HANDSHAKE_RES_BAD_REQUEST);
    return 0;
}

static const char*
harmony_websock_find_header(HarmonyWebsockHTTPHeader *hdrs,
                            size_t nhdrs,
                            const char *name)
{
    size_t i;

    for (i = 0; i < nhdrs; i++) {
        if (g_str_equal(hdrs[i].name, name)) {
            return hdrs[i].value;
        }
    }

    return NULL;
}


static void harmony_websock_handshake_send_res_ok(HarmonyWebsock *hw,
                                                const char *key,
                                                const bool use_protocols,
                                                Error **errp)
{
    char combined_key[HARMONY_WEBSOCK_CLIENT_KEY_LEN +
                      HARMONY_WEBSOCK_GUID_LEN + 1];
    char *accept = NULL;
    char *date = NULL;

    g_strlcpy(combined_key, key, HARMONY_WEBSOCK_CLIENT_KEY_LEN + 1);
    g_strlcat(combined_key, HARMONY_WEBSOCK_GUID,
              HARMONY_WEBSOCK_CLIENT_KEY_LEN +
              HARMONY_WEBSOCK_GUID_LEN + 1);

    /* hash and encode it */
    if (qcrypto_hash_base64(QCRYPTO_HASH_ALGO_SHA1,
                            combined_key,
                            HARMONY_WEBSOCK_CLIENT_KEY_LEN +
                            HARMONY_WEBSOCK_GUID_LEN,
                            &accept,
                            errp) < 0) {
        harmony_websock_handshake_send_res_err(
            hw, HARMONY_WEBSOCK_HANDSHAKE_RES_SERVER_ERR);
        return;
    }

    date = harmony_websock_date_str();
    if (use_protocols) {
            harmony_websock_handshake_send_res(
                hw, HARMONY_WEBSOCK_HANDSHAKE_WITH_PROTO_RES_OK,
                date, accept);
    } else {
            harmony_websock_handshake_send_res(
                hw, HARMONY_WEBSOCK_HANDSHAKE_RES_OK, date, accept);
    }

    g_free(date);
    g_free(accept);
}

static void harmony_websock_handshake_process(HarmonyWebsock *hw,
                                            char *buffer,
                                            Error **errp)
{
    HarmonyWebsockHTTPHeader hdrs[32];
    size_t nhdrs = G_N_ELEMENTS(hdrs);
    const char *protocols = NULL, *version = NULL, *key = NULL,
        *host = NULL, *connection = NULL, *upgrade = NULL;
    char **connectionv;
    bool upgraded = false;
    size_t i;

    nhdrs = harmony_websock_extract_headers(hw, buffer, hdrs, nhdrs, errp);
    if (!nhdrs) {
        return;
    }

    protocols = harmony_websock_find_header(
        hdrs, nhdrs, HARMONY_WEBSOCK_HEADER_PROTOCOL);

    version = harmony_websock_find_header(
        hdrs, nhdrs, HARMONY_WEBSOCK_HEADER_VERSION);
    if (!version) {
        error_setg(errp, "Missing websocket version header data");
        goto bad_request;
    }

    key = harmony_websock_find_header(
        hdrs, nhdrs, HARMONY_WEBSOCK_HEADER_KEY);
    if (!key) {
        error_setg(errp, "Missing websocket key header data");
        goto bad_request;
    }

    host = harmony_websock_find_header(
        hdrs, nhdrs, HARMONY_WEBSOCK_HEADER_HOST);
    if (!host) {
        error_setg(errp, "Missing websocket host header data");
        goto bad_request;
    }

    connection = harmony_websock_find_header(
        hdrs, nhdrs, HARMONY_WEBSOCK_HEADER_CONNECTION);
    if (!connection) {
        error_setg(errp, "Missing websocket connection header data");
        goto bad_request;
    }

    upgrade = harmony_websock_find_header(
        hdrs, nhdrs, HARMONY_WEBSOCK_HEADER_UPGRADE);
    if (!upgrade) {
        error_setg(errp, "Missing websocket upgrade header data");
        goto bad_request;
    }

    if (protocols) {
            if (!g_strrstr(protocols, HARMONY_WEBSOCK_PROTOCOL_BINARY)) {
                error_setg(errp, "No '%s' protocol is supported by client '%s'",
                           HARMONY_WEBSOCK_PROTOCOL_BINARY, protocols);
                goto bad_request;
            }
    }

    if (!g_str_equal(version, HARMONY_WEBSOCK_SUPPORTED_VERSION)) {
        error_setg(errp, "Version '%s' is not supported by client '%s'",
                   HARMONY_WEBSOCK_SUPPORTED_VERSION, version);
        goto bad_request;
    }

    if (strlen(key) != HARMONY_WEBSOCK_CLIENT_KEY_LEN) {
        error_setg(errp, "Key length '%zu' was not as expected '%d'",
                   strlen(key), HARMONY_WEBSOCK_CLIENT_KEY_LEN);
        goto bad_request;
    }

    connectionv = g_strsplit(connection, ",", 0);
    for (i = 0; connectionv != NULL && connectionv[i] != NULL; i++) {
        g_strstrip(connectionv[i]);
        if (strcasecmp(connectionv[i],
                       HARMONY_WEBSOCK_CONNECTION_UPGRADE) == 0) {
            upgraded = true;
        }
    }
    g_strfreev(connectionv);
    if (!upgraded) {
        error_setg(errp, "No connection upgrade requested '%s'", connection);
        goto bad_request;
    }

    if (strcasecmp(upgrade, HARMONY_WEBSOCK_UPGRADE_WEBSOCKET) != 0) {
        error_setg(errp, "Incorrect upgrade method '%s'", upgrade);
        goto bad_request;
    }

    harmony_websock_handshake_send_res_ok(hw, key, !!protocols, errp);
    return;

 bad_request:
    harmony_websock_handshake_send_res_err(
        hw, HARMONY_WEBSOCK_HANDSHAKE_RES_BAD_REQUEST);
}

static int harmony_websock_handshake_read(HarmonyWebsock *hw,
                                        Error **errp)
{
    char *handshake_end;
    ssize_t ret;
    size_t want = 4096 - hw->encinput.offset;

    buffer_reserve(&hw->encinput, want);
    ret = qio_channel_read(hw->master,
                           (char *)buffer_end(&hw->encinput), want, errp);
    if (ret < 0) {
        return -1;
    }
    hw->encinput.offset += ret;

    handshake_end = g_strstr_len((char *)hw->encinput.buffer,
                                 hw->encinput.offset,
                                 HARMONY_WEBSOCK_HANDSHAKE_END);
    if (!handshake_end) {
        if (hw->encinput.offset >= 4096) {
            harmony_websock_handshake_send_res_err(
                hw, HARMONY_WEBSOCK_HANDSHAKE_RES_TOO_LARGE);
            error_setg(errp,
                       "End of headers not found in first 4096 bytes");
            return 1;
        } else if (ret == 0) {
            error_setg(errp,
                       "End of headers not found before connection closed");
            return -1;
        }
        return 0;
    }
    *handshake_end = '\0';

    harmony_websock_handshake_process(hw,
                                    (char *)hw->encinput.buffer,
                                    errp);

    buffer_advance(&hw->encinput,
                   handshake_end - (char *)hw->encinput.buffer +
                   strlen(HARMONY_WEBSOCK_HANDSHAKE_END));
    return 1;
}

static gboolean harmony_websock_handshake_send(QIOChannel *ioc,
                                            GIOCondition condition,
                                            gpointer user_data)
{
    QIOTask *task = user_data;
    HarmonyWebsock *hw = HARMONY_WEBSOCK(
        qio_task_get_source(task));
    Error *err = NULL;
    ssize_t ret;

    ret = qio_channel_write(hw->master,
                            (char *)hw->encoutput.buffer,
                            hw->encoutput.offset,
                            &err);

    if (ret < 0) {
        qio_task_set_error(task, err);
        qio_task_complete(task);
        return FALSE;
    }

    buffer_advance(&hw->encoutput, ret);
    if (hw->encoutput.offset == 0) {
        if (hw->io_err) {
            qio_task_set_error(task, hw->io_err);
            hw->io_err = NULL;
            qio_task_complete(task);
        } else {
            qio_task_complete(task);
        }
        return FALSE;
    }

    return TRUE;
}

static gboolean harmony_websock_handshake_io(QIOChannel *ioc,
                                            GIOCondition condition,
                                            gpointer user_data)
{
    QIOTask *task = user_data;
    HarmonyWebsock *hw = HARMONY_WEBSOCK(
        qio_task_get_source(task));
    Error *err = NULL;
    int ret;

    ret = harmony_websock_handshake_read(hw, &err);
    if (ret < 0) {
        qio_task_set_error(task, err);
        qio_task_complete(task);
        return FALSE;
    }
    if (ret == 0) {
        return TRUE;
    }

    error_propagate(&hw->io_err, err);

    qio_channel_add_watch(
        hw->master,
        G_IO_OUT,
        harmony_websock_handshake_send,
        task,
        NULL);
    return FALSE;
}


static void harmony_websock_encode(HarmonyWebsock *hw,
                                uint8_t opcode,
                                const struct iovec *iov,
                                size_t niov,
                                size_t size)
{
    size_t header_size;
    size_t i;
    union {
        char buf[HARMONY_WEBSOCK_HEADER_LEN_64_BIT];
        HarmonyWebsockHeader ws;
    } header;

    assert(size <= iov_size(iov, niov));

    header.ws.b0 = HARMONY_WEBSOCK_HEADER_FIELD_FIN |
        (opcode & HARMONY_WEBSOCK_HEADER_FIELD_OPCODE);
    if (size < HARMONY_WEBSOCK_PAYLOAD_LEN_THRESHOLD_7_BIT) {
        header.ws.b1 = (uint8_t)size;
        header_size = HARMONY_WEBSOCK_HEADER_LEN_7_BIT;
    } else if (size < HARMONY_WEBSOCK_PAYLOAD_LEN_THRESHOLD_16_BIT) {
        header.ws.b1 = HARMONY_WEBSOCK_PAYLOAD_LEN_MAGIC_16_BIT;
        header.ws.u.s16.l16 = cpu_to_be16((uint16_t)size);
        header_size = HARMONY_WEBSOCK_HEADER_LEN_16_BIT;
    } else {
        header.ws.b1 = HARMONY_WEBSOCK_PAYLOAD_LEN_MAGIC_64_BIT;
        header.ws.u.s64.l64 = cpu_to_be64(size);
        header_size = HARMONY_WEBSOCK_HEADER_LEN_64_BIT;
    }
    header_size -= HARMONY_WEBSOCK_HEADER_LEN_MASK;

    buffer_reserve(&hw->encoutput, header_size + size);
    buffer_append(&hw->encoutput, header.buf, header_size);
    for (i = 0; i < niov && size != 0; i++) {
        size_t want = iov[i].iov_len;
        if (want > size) {
            want = size;
        }
        buffer_append(&hw->encoutput, iov[i].iov_base, want);
        size -= want;
    }
}


static ssize_t harmony_websock_write_wire(HarmonyWebsock *, Error **);


static void harmony_websock_write_close(HarmonyWebsock *hw,
                                        uint16_t code, 
                                        const char *reason)
{
    struct iovec iov[2] = {
        { .iov_base = &code, .iov_len = sizeof(code) },
    };
    size_t niov = 1;
    size_t size = iov[0].iov_len;

    cpu_to_be16s(&code);

    if (reason) {
        iov[1].iov_base = (void *)reason;
        iov[1].iov_len = strlen(reason);
        size += iov[1].iov_len;
        niov++;
    }
    harmony_websock_encode(hw, HARMONY_WEBSOCK_OPCODE_CLOSE,
                        iov, niov, size);
    harmony_websock_write_wire(hw, NULL);
    qio_channel_shutdown(hw->master, QIO_CHANNEL_SHUTDOWN_BOTH, NULL);
}


static int harmony_websock_decode_header(HarmonyWebsock *hw,
                                        Error **errp)
{
    unsigned char opcode, fin, has_mask;
    size_t header_size;
    size_t payload_len;
    HarmonyWebsockHeader *header =
        (HarmonyWebsockHeader *)hw->encinput.buffer;

    if (hw->payload_remain) {
        error_setg(errp,
                   "Decoding header but %zu bytes of payload remain",
                   hw->payload_remain);
        harmony_websock_write_close(
            hw, HARMONY_WEBSOCK_STATUS_SERVER_ERR,
            "internal server error");
        return -1;
    }
    if (hw->encinput.offset < HARMONY_WEBSOCK_HEADER_LEN_7_BIT) {
        return QIO_CHANNEL_ERR_BLOCK;
    }

    fin = header->b0 & HARMONY_WEBSOCK_HEADER_FIELD_FIN;
    opcode = header->b0 & HARMONY_WEBSOCK_HEADER_FIELD_OPCODE;
    has_mask = header->b1 & HARMONY_WEBSOCK_HEADER_FIELD_HAS_MASK;
    payload_len = header->b1 & HARMONY_WEBSOCK_HEADER_FIELD_PAYLOAD_LEN;

    if (opcode) {
        hw->opcode = opcode;
    } else {
        opcode = hw->opcode;
    }

    if (opcode == HARMONY_WEBSOCK_OPCODE_CLOSE) {
        return 0;
    }

    if (!fin) {
        if (opcode != HARMONY_WEBSOCK_OPCODE_TEXT_FRAME && 
            opcode != HARMONY_WEBSOCK_OPCODE_BINARY_FRAME) {
            error_setg(errp, "only text and binary websocket frames may be fragmented");
            harmony_websock_write_close(
                hw, HARMONY_WEBSOCK_STATUS_POLICY,
                "only text and binary frames may be fragmented");
            return -1;
        }
    } else {
        if (opcode != HARMONY_WEBSOCK_OPCODE_TEXT_FRAME && 
            opcode != HARMONY_WEBSOCK_OPCODE_BINARY_FRAME &&
            opcode != HARMONY_WEBSOCK_OPCODE_CLOSE &&
            opcode != HARMONY_WEBSOCK_OPCODE_PING &&
            opcode != HARMONY_WEBSOCK_OPCODE_PONG) {
            error_setg(errp, "unsupported opcode: 0x%04x; only text, binary, close, "
                       "ping, and pong websocket frames are supported", opcode);
            harmony_websock_write_close(
                hw, HARMONY_WEBSOCK_STATUS_INVALID_DATA,
                "only text, binary, close, ping, and pong frames are supported");
            return -1;
        }
    }
    if (!has_mask) {
        error_setg(errp, "client websocket frames must be masked");
        harmony_websock_write_close(
            hw, HARMONY_WEBSOCK_STATUS_PROTOCOL_ERR,
            "client frames must be masked");
        return -1;
    }

    if (payload_len < HARMONY_WEBSOCK_PAYLOAD_LEN_MAGIC_16_BIT) {
        hw->payload_remain = payload_len;
        header_size = HARMONY_WEBSOCK_HEADER_LEN_7_BIT;
        hw->mask = header->u.m;
    } else if (opcode & HARMONY_WEBSOCK_CONTROL_OPCODE_MASK) {
        error_setg(errp, "websocket control frame is too large");
        harmony_websock_write_close(
            hw, HARMONY_WEBSOCK_STATUS_PROTOCOL_ERR,
            "control frame is too large");
        return -1;
    } else if (payload_len == HARMONY_WEBSOCK_PAYLOAD_LEN_MAGIC_16_BIT &&
               hw->encinput.offset >= HARMONY_WEBSOCK_HEADER_LEN_16_BIT) {
        hw->payload_remain = be16_to_cpu(header->u.s16.l16);
        header_size = HARMONY_WEBSOCK_HEADER_LEN_16_BIT;
        hw->mask = header->u.s16.m16;
    } else if (payload_len == HARMONY_WEBSOCK_PAYLOAD_LEN_MAGIC_64_BIT &&
               hw->encinput.offset >= HARMONY_WEBSOCK_HEADER_LEN_64_BIT) {
        hw->payload_remain = be64_to_cpu(header->u.s64.l64);
        header_size = HARMONY_WEBSOCK_HEADER_LEN_64_BIT;
        hw->mask = header->u.s64.m64;
    } else {
        return QIO_CHANNEL_ERR_BLOCK;
    }

    buffer_advance(&hw->encinput, header_size);
    return 0;
}


static int harmony_websock_decode_payload(HarmonyWebsock *hw,
                                        Error **errp)
{
    size_t i;
    size_t payload_len = 0;
    uint32_t *payload32;

    if (hw->payload_remain) {
        if (hw->encinput.offset < hw->payload_remain) {
            if (hw->opcode & HARMONY_WEBSOCK_CONTROL_OPCODE_MASK) {
                return QIO_CHANNEL_ERR_BLOCK;
            }
            payload_len = hw->encinput.offset - (hw->encinput.offset % 4);
        } else {
            payload_len = hw->payload_remain;
        }
        if (payload_len == 0) {
            return QIO_CHANNEL_ERR_BLOCK;
        }

        hw->payload_remain -= payload_len;

        payload32 = (uint32_t *)hw->encinput.buffer;
        for (i = 0; i < payload_len / 4; i++) {
            payload32[i] ^= hw->mask.u;
        }
        
        for (i *= 4; i < payload_len; i++) {
            hw->encinput.buffer[i] ^= hw->mask.c[i % 4];
        }
    }

    if (hw->opcode == HARMONY_WEBSOCK_OPCODE_TEXT_FRAME || 
        hw->opcode == HARMONY_WEBSOCK_OPCODE_BINARY_FRAME) {
        if (payload_len) {
            buffer_reserve(&hw->rawinput, payload_len);
            buffer_append(&hw->rawinput, hw->encinput.buffer, payload_len);
        }
    } else if (hw->opcode == HARMONY_WEBSOCK_OPCODE_CLOSE) {
        error_setg(errp, "websocket closed by peer");
        if (payload_len) {
            struct iovec iov = { .iov_base = hw->encinput.buffer,
                                 .iov_len = hw->encinput.offset };
            harmony_websock_encode(hw, HARMONY_WEBSOCK_OPCODE_CLOSE,
                                &iov, 1, iov.iov_len);
            harmony_websock_write_wire(hw, NULL);
            qio_channel_shutdown(hw->master, QIO_CHANNEL_SHUTDOWN_BOTH, NULL);
        } else {
            harmony_websock_write_close(
                hw, HARMONY_WEBSOCK_STATUS_NORMAL, "peer requested close");
        }
        return -1;
    } else if (hw->opcode == HARMONY_WEBSOCK_OPCODE_PING) {
        if (hw->pong_remain == 0) {
            struct iovec iov = { .iov_base = hw->encinput.buffer,
                                 .iov_len = hw->encinput.offset };
            harmony_websock_encode(hw, HARMONY_WEBSOCK_OPCODE_PONG,
                                &iov, 1, iov.iov_len);
            hw->pong_remain = hw->encoutput.offset;
        }
    } 

    if (payload_len) {
        buffer_advance(&hw->encinput, payload_len);
    }
    return 0;
}

HarmonyWebsock*
harmony_websock_new_server(QIOChannel *master, 
                        HarmonyWebsockCheckPathFunc path_checker, 
                        void *opaque)
{
    HarmonyWebsock *hw;
    QIOChannel *ioc;

    hw = HARMONY_WEBSOCK(object_new(TYPE_HARMONY_WEBSOCK));
    hw->path_checker = path_checker;
    hw->opaque = opaque;

    ioc = QIO_CHANNEL(hw);

    hw->master = master;
    if (qio_channel_has_feature(master, QIO_CHANNEL_FEATURE_SHUTDOWN)) {
        qio_channel_set_feature(ioc, QIO_CHANNEL_FEATURE_SHUTDOWN);
    }
    object_ref(OBJECT(master));

    return hw;
}

void harmony_websock_handshake(HarmonyWebsock *hw,
                            QIOTaskFunc func,
                            gpointer opaque,
                            GDestroyNotify destroy)
{
    QIOTask *task;

    task = qio_task_new(OBJECT(hw),
                        func,
                        opaque,
                        destroy);

    qio_channel_add_watch(hw->master,
                          G_IO_IN,
                          harmony_websock_handshake_io,
                          task,
                          NULL);
}


static void harmony_websock_finalize(Object *obj)
{
    HarmonyWebsock *hw = HARMONY_WEBSOCK(obj);

    buffer_free(&hw->encinput);
    buffer_free(&hw->encoutput);
    buffer_free(&hw->rawinput);
    object_unref(OBJECT(hw->master));
    if (hw->io_tag) {
        g_source_remove(hw->io_tag);
    }
    if (hw->io_err) {
        error_free(hw->io_err);
    }

    free(hw->path);
}


static ssize_t harmony_websock_read_wire(HarmonyWebsock *hw,
                                        Error **errp)
{
    ssize_t ret;

    if (hw->encinput.offset < HARMONY_WEBSOCK_MAX_BUFFER) {
        size_t want = HARMONY_WEBSOCK_MAX_BUFFER - hw->encinput.offset;

        buffer_reserve(&hw->encinput, want);
        ret = qio_channel_read(hw->master,
                               (char *)hw->encinput.buffer +
                               hw->encinput.offset,
                               want,
                               errp);
        if (ret < 0) {
            return ret;
        }
        if (ret == 0 && hw->encinput.offset == 0) {
            hw->io_eof = TRUE;
            return 0;
        }
        hw->encinput.offset += ret;
    }

    while (hw->encinput.offset != 0) {
        if (hw->payload_remain == 0) {
            ret = harmony_websock_decode_header(hw, errp);
            if (ret < 0) {
                return ret;
            }
        }

        ret = harmony_websock_decode_payload(hw, errp);
        if (ret < 0) {
            return ret;
        }
    }
    return 1;
}


static ssize_t harmony_websock_write_wire(HarmonyWebsock *hw,
                                        Error **errp)
{
    ssize_t ret;
    ssize_t done = 0;

    while (hw->encoutput.offset > 0) {
        ret = qio_channel_write(hw->master,
                                (char *)hw->encoutput.buffer,
                                hw->encoutput.offset,
                                errp);
        if (ret < 0) {
            if (ret == QIO_CHANNEL_ERR_BLOCK &&
                done > 0) {
                return done;
            } else {
                return ret;
            }
        }
        buffer_advance(&hw->encoutput, ret);
        done += ret;
        if (hw->pong_remain < ret) {
            hw->pong_remain = 0;
        } else {
            hw->pong_remain -= ret;
        }
    }
    return done;
}


static void harmony_websock_flush_free(gpointer user_data)
{
    HarmonyWebsock *hw = HARMONY_WEBSOCK(user_data);
    object_unref(OBJECT(hw));
}

static void harmony_websock_set_watch(HarmonyWebsock *hw);

static gboolean harmony_websock_flush(QIOChannel *ioc,
                                    GIOCondition condition,
                                    gpointer user_data)
{
    HarmonyWebsock *hw = HARMONY_WEBSOCK(user_data);
    ssize_t ret;

    if (condition & G_IO_OUT) {
        ret = harmony_websock_write_wire(hw, &hw->io_err);
        if (ret < 0) {
            goto cleanup;
        }
    }

    if (condition & G_IO_IN) {
        ret = harmony_websock_read_wire(hw, &hw->io_err);
        if (ret < 0) {
            goto cleanup;
        }
    }

 cleanup:
    harmony_websock_set_watch(hw);
    return FALSE;
}


static void harmony_websock_unset_watch(HarmonyWebsock *hw)
{
    if (hw->io_tag) {
        g_source_remove(hw->io_tag);
        hw->io_tag = 0;
    }
}

static void harmony_websock_set_watch(HarmonyWebsock *hw)
{
    GIOCondition cond = 0;

    harmony_websock_unset_watch(hw);

    if (hw->io_err) {
        return;
    }

    if (hw->encoutput.offset) {
        cond |= G_IO_OUT;
    }
    if (hw->encinput.offset < HARMONY_WEBSOCK_MAX_BUFFER 
        && !hw->io_eof) {
        cond |= G_IO_IN;
    }

    if (cond) {
        object_ref(OBJECT(hw));
        hw->io_tag =
            qio_channel_add_watch(hw->master,
                                  cond,
                                  harmony_websock_flush,
                                  hw,
                                  harmony_websock_flush_free);
    }
}


static ssize_t harmony_websock_readv(QIOChannel *ioc,
                                    const struct iovec *iov,
                                    size_t niov,
                                    int **fds,
                                    size_t *nfds,
                                    int flags,
                                    Error **errp)
{
    HarmonyWebsock *hw = HARMONY_WEBSOCK(ioc);
    size_t i;
    ssize_t got = 0;
    ssize_t ret;

    if (hw->io_err) {
        error_propagate(errp, error_copy(hw->io_err));
        return -1;
    }

    if (!hw->rawinput.offset) {
        ret = harmony_websock_read_wire(hw, errp);
        if (ret < 0) {
            return ret;
        }
    }

    for (i = 0 ; i < niov ; i++) {
        size_t want = iov[i].iov_len;
        if (want > (hw->rawinput.offset - got)) {
            want = (hw->rawinput.offset - got);
        }

        memcpy(iov[i].iov_base,
               hw->rawinput.buffer + got,
               want);
        got += want;

        if (want < iov[i].iov_len) {
            break;
        }
    }

    buffer_advance(&hw->rawinput, got);
    harmony_websock_set_watch(hw);
    return got;
}


static ssize_t harmony_websock_writev(QIOChannel *ioc,
                                    const struct iovec *iov,
                                    size_t niov,
                                    int *fds,
                                    size_t nfds,
                                    int flags,
                                    Error **errp)
{
    HarmonyWebsock *hw = HARMONY_WEBSOCK(ioc);
    ssize_t want = iov_size(iov, niov);
    ssize_t avail;
    ssize_t ret;

    if (hw->io_err) {
        error_propagate(errp, error_copy(hw->io_err));
        return -1;
    }

    if (hw->io_eof) {
        error_setg(errp, "%s", "Broken pipe");
        return -1;
    }

    avail = hw->encoutput.offset > 0 ? 0 : HARMONY_WEBSOCK_MAX_BUFFER;
    if (want > avail) {
        want = avail;
    }

    if (want) {
        harmony_websock_encode(hw,
                            HARMONY_WEBSOCK_OPCODE_BINARY_FRAME,
                            iov, niov, want);
    }

    ret = harmony_websock_write_wire(hw, errp);
    if (ret < 0 &&
        ret != QIO_CHANNEL_ERR_BLOCK) {
        harmony_websock_unset_watch(hw);
        return -1;
    }

    harmony_websock_set_watch(hw);

    if (want == 0) {
        return QIO_CHANNEL_ERR_BLOCK;
    }

    return want;
}

static int harmony_websock_set_blocking(QIOChannel *ioc,
                                        bool enabled,
                                        Error **errp)
{
    HarmonyWebsock *hw = HARMONY_WEBSOCK(ioc);

    qio_channel_set_blocking(hw->master, enabled, errp);
    return 0;
}

static void harmony_websock_set_delay(QIOChannel *ioc,
                                    bool enabled)
{
    HarmonyWebsock *hw = HARMONY_WEBSOCK(ioc);

    qio_channel_set_delay(hw->master, enabled);
}

static void harmony_websock_set_cork(QIOChannel *ioc,
                                    bool enabled)
{
    HarmonyWebsock *hw = HARMONY_WEBSOCK(ioc);

    qio_channel_set_cork(hw->master, enabled);
}

static int harmony_websock_shutdown(QIOChannel *ioc,
                                    QIOChannelShutdown how,
                                    Error **errp)
{
    HarmonyWebsock *hw = HARMONY_WEBSOCK(ioc);

    return qio_channel_shutdown(hw->master, how, errp);
}

static int harmony_websock_close(QIOChannel *ioc,
                                Error **errp)
{
    HarmonyWebsock *hw = HARMONY_WEBSOCK(ioc);

    return qio_channel_close(hw->master, errp);
}

typedef struct HarmonyWebsockSource HarmonyWebsockSource;
struct HarmonyWebsockSource {
    GSource parent;
    HarmonyWebsock *hw;
    GIOCondition condition;
};

static gboolean
harmony_websock_source_check(GSource *source)
{
    HarmonyWebsockSource *hwsource = (HarmonyWebsockSource *)source;
    GIOCondition cond = 0;

    if (hwsource->hw->rawinput.offset) {
        cond |= G_IO_IN;
    }
    if (!hwsource->hw->encoutput.offset) {
        cond |= G_IO_OUT;
    }
    if (hwsource->hw->io_eof) {
        cond |= G_IO_HUP;
    }
    if (hwsource->hw->io_err) {
        cond |= G_IO_ERR;
    }

    return cond & hwsource->condition;
}

static gboolean
harmony_websock_source_prepare(GSource *source,
                            gint *timeout)
{
    *timeout = -1;
    return harmony_websock_source_check(source);
}

static gboolean
harmony_websock_source_dispatch(GSource *source,
                                GSourceFunc callback,
                                gpointer user_data)
{
    QIOChannelFunc func = (QIOChannelFunc)callback;
    HarmonyWebsockSource *hwsource = (HarmonyWebsockSource *)source;

    return (*func)(QIO_CHANNEL(hwsource->hw),
                   harmony_websock_source_check(source),
                   user_data);
}

static void
harmony_websock_source_finalize(GSource *source)
{
    HarmonyWebsockSource *hwsource = (HarmonyWebsockSource *)source;

    object_unref(OBJECT(hwsource->hw));
}

GSourceFuncs harmony_websock_source_funcs = {
    harmony_websock_source_prepare,
    harmony_websock_source_check,
    harmony_websock_source_dispatch,
    harmony_websock_source_finalize
};

static GSource *harmony_websock_create_watch(QIOChannel *ioc,
                                            GIOCondition condition)
{
    HarmonyWebsock *hw = HARMONY_WEBSOCK(ioc);
    HarmonyWebsockSource *hwsource;
    GSource *source;

    source = g_source_new(&harmony_websock_source_funcs,
                          sizeof(HarmonyWebsockSource));
    hwsource = (HarmonyWebsockSource *)source;

    hwsource->hw = hw;
    object_ref(OBJECT(hw));

    hwsource->condition = condition;

    harmony_websock_set_watch(hw);
    return source;
}

static void harmony_websock_class_init(ObjectClass *klass,
                                    const void *class_data G_GNUC_UNUSED)
{
    QIOChannelClass *ioc_klass = QIO_CHANNEL_CLASS(klass);

    ioc_klass->io_writev = harmony_websock_writev;
    ioc_klass->io_readv = harmony_websock_readv;
    ioc_klass->io_set_blocking = harmony_websock_set_blocking;
    ioc_klass->io_set_cork = harmony_websock_set_cork;
    ioc_klass->io_set_delay = harmony_websock_set_delay;
    ioc_klass->io_close = harmony_websock_close;
    ioc_klass->io_shutdown = harmony_websock_shutdown;
    ioc_klass->io_create_watch = harmony_websock_create_watch;
}

static const TypeInfo harmony_websock_info = {
    .parent = TYPE_QIO_CHANNEL,
    .name = TYPE_HARMONY_WEBSOCK,
    .instance_size = sizeof(HarmonyWebsock),
    .instance_finalize = harmony_websock_finalize,
    .class_init = harmony_websock_class_init,
};

static void harmony_websock_register_types(void)
{
    type_register_static(&harmony_websock_info);
}

type_init(harmony_websock_register_types);
