
#include "qemu/osdep.h"
#include "sysemu/sysemu.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/vnc_keysym.h"
#include "input-handler.h"
#include "qemu/error-report.h"
#define HARMONY_SERVER_INPUT_MULTITOUCH 0
#define HARMONY_SERVER_INPUT_KEY        1
#define INPUT_SIZE                      6
static uint8_t read_u8(uint8_t *data, size_t offset)
{
    return data[offset];
}

static uint16_t read_u16(uint8_t *data, size_t offset)
{
    return ((data[offset] & 0xFF) << 8) | (data[offset + 1] & 0xFF);
}

static uint32_t read_u32(uint8_t *data, size_t offset)
{
    return ((data[offset] << 24) | (data[offset + 1] << 16) |
            (data[offset + 2] << 8) | data[offset + 3]);
}

static void pointer_type_change_notifier(Notifier *notifier, void *data)
{
    HarmonyClientInput *hci = container_of(notifier, HarmonyClientInput, mouse_mode_notifier);
    HarmonyServerInput *hsi = hci->hsi;
    QemuConsole *con = hsi->hsd->dcl.con;

    hci->multitouch = qemu_input_is_multitouch(con);
}

static void multitouch_event(HarmonyClientInput *hci, uint8_t bmask, int x, int y) 
{
    HarmonyServerInput *hsi = hci->hsi;
    QemuConsole *con = hsi->hsd->dcl.con;

    if (!hci->multitouch) {
        return;
    }

    if (bmask > 0) {
        if (hci->last_bmask < 1) {
            qemu_input_queue_mtt(con, INPUT_MULTI_TOUCH_TYPE_UPDATE, 0, 0);
        } else {
            qemu_input_queue_mtt(con, INPUT_MULTI_TOUCH_TYPE_BEGIN, 0, 0);
        }

        qemu_input_queue_btn(con, INPUT_BUTTON_TOUCH, true);

        qemu_input_queue_mtt_abs(con,
                                INPUT_AXIS_X,  x,
                                0, hci->hsi->hsd->width,
                                0, 0);
        qemu_input_queue_mtt_abs(con,
                                INPUT_AXIS_Y, y,
                                0, hci->hsi->hsd->height,
                                0, 0);
    } else {
        if (bmask == 0 && hci->last_bmask > 0) {
            qemu_input_queue_mtt(con, INPUT_MULTI_TOUCH_TYPE_END, 0, -1);
        }
    }

    hci->last_bmask = bmask;
    qemu_input_event_sync();
}

static void do_key_event(HarmonyClientInput *hci, int down, int keycode, int sym)
{
    HarmonyServerInput *hsi = hci->hsi;
    QemuConsole *con = hsi->hsd->dcl.con;
    QKeyCode qcode = qemu_input_key_number_to_qcode(keycode);

    switch (qcode) {
    case Q_KEY_CODE_1 ... Q_KEY_CODE_9: /* '1' to '9' keys */
        if (con == NULL && down &&
            qkbd_state_modifier_get(hci->hsi->kbd, QKBD_MOD_CTRL) &&
            qkbd_state_modifier_get(hci->hsi->kbd, QKBD_MOD_ALT)) {
            /* Reset the modifiers sent to the current console */
            qkbd_state_lift_all_keys(hci->hsi->kbd);
            console_select(qcode - Q_KEY_CODE_1);
            return;
        }
    default:
        break;
    }

    qkbd_state_key_event(hci->hsi->kbd, qcode, down);
    if (!qemu_console_is_graphic(NULL)) {
        bool numlock = qkbd_state_modifier_get(hci->hsi->kbd, QKBD_MOD_NUMLOCK);
        bool control = qkbd_state_modifier_get(hci->hsi->kbd, QKBD_MOD_CTRL);
        /* QEMU console emulation */
        if (down) {
            switch (keycode) {
            case 0x2a:                          /* Left Shift */
            case 0x36:                          /* Right Shift */
            case 0x1d:                          /* Left CTRL */
            case 0x9d:                          /* Right CTRL */
            case 0x38:                          /* Left ALT */
            case 0xb8:                          /* Right ALT */
                break;
            case 0xc8:
                qemu_text_console_put_keysym(NULL, QEMU_KEY_UP);
                break;
            case 0xd0:
                qemu_text_console_put_keysym(NULL, QEMU_KEY_DOWN);
                break;
            case 0xcb:
                qemu_text_console_put_keysym(NULL, QEMU_KEY_LEFT);
                break;
            case 0xcd:
                qemu_text_console_put_keysym(NULL, QEMU_KEY_RIGHT);
                break;
            case 0xd3:
                qemu_text_console_put_keysym(NULL, QEMU_KEY_DELETE);
                break;
            case 0xc7:
                qemu_text_console_put_keysym(NULL, QEMU_KEY_HOME);
                break;
            case 0xcf:
                qemu_text_console_put_keysym(NULL, QEMU_KEY_END);
                break;
            case 0xc9:
                qemu_text_console_put_keysym(NULL, QEMU_KEY_PAGEUP);
                break;
            case 0xd1:
                qemu_text_console_put_keysym(NULL, QEMU_KEY_PAGEDOWN);
                break;

            case 0x47:
                qemu_text_console_put_keysym(NULL, numlock ? '7' : QEMU_KEY_HOME);
                break;
            case 0x48:
                qemu_text_console_put_keysym(NULL, numlock ? '8' : QEMU_KEY_UP);
                break;
            case 0x49:
                qemu_text_console_put_keysym(NULL, numlock ? '9' : QEMU_KEY_PAGEUP);
                break;
            case 0x4b:
                qemu_text_console_put_keysym(NULL, numlock ? '4' : QEMU_KEY_LEFT);
                break;
            case 0x4c:
                qemu_text_console_put_keysym(NULL, '5');
                break;
            case 0x4d:
                qemu_text_console_put_keysym(NULL, numlock ? '6' : QEMU_KEY_RIGHT);
                break;
            case 0x4f:
                qemu_text_console_put_keysym(NULL, numlock ? '1' : QEMU_KEY_END);
                break;
            case 0x50:
                qemu_text_console_put_keysym(NULL, numlock ? '2' : QEMU_KEY_DOWN);
                break;
            case 0x51:
                qemu_text_console_put_keysym(NULL, numlock ? '3' : QEMU_KEY_PAGEDOWN);
                break;
            case 0x52:
                qemu_text_console_put_keysym(NULL, '0');
                break;
            case 0x53:
                qemu_text_console_put_keysym(NULL, numlock ? '.' : QEMU_KEY_DELETE);
                break;

            case 0xb5:
                qemu_text_console_put_keysym(NULL, '/');
                break;
            case 0x37:
                qemu_text_console_put_keysym(NULL, '*');
                break;
            case 0x4a:
                qemu_text_console_put_keysym(NULL, '-');
                break;
            case 0x4e:
                qemu_text_console_put_keysym(NULL, '+');
                break;
            case 0x9c:
                qemu_text_console_put_keysym(NULL, '\n');
                break;

            default:
                if (control) {
                    qemu_text_console_put_keysym(NULL, sym & 0x1f);
                } else {
                    qemu_text_console_put_keysym(NULL, sym);
                }
                break;
            }
        }
    }
}

static void key_event(HarmonyClientInput *hci, int down, uint32_t sym)
{
    int keycode;
    int lsym = sym;

    if (lsym >= 'A' && lsym <= 'Z' && qemu_console_is_graphic(NULL)) {
        lsym = lsym - 'A' + 'a';
    }

    keycode = keysym2scancode(hci->hsi->kbd_layout, lsym & 0xFFFF,
                              hci->hsi->kbd, down) & SCANCODE_KEYMASK;
    do_key_event(hci, down, keycode, sym);
}

static gboolean harmony_server_input_connect(HarmonyClient *hc, void *opaque)
{
    HarmonyServerInput *hsi = opaque;
    HarmonyClientInput *hci = g_new0(HarmonyClientInput, 1);

    hci->hsi = hsi;
    hci->hc = hc;
    hci->multitouch = true;

    hci->mouse_mode_notifier.notify = pointer_type_change_notifier;
    qemu_add_mouse_mode_change_notifier(&hci->mouse_mode_notifier);

    QTAILQ_INSERT_TAIL(&hsi->clients, hci, next);

    return TRUE;
}

static int harmony_server_input_read(HarmonyClient *hc, void *opaque, 
                                    uint8_t *data, size_t len) 
{
    HarmonyServerInput *hsi = opaque;
    HarmonyClientInput *hci;
    bool found = false;
    if (len % INPUT_SIZE != 0) {
        error_report("invalid harmony server input len:%ld", len);
        return len;
    }

    QTAILQ_FOREACH(hci, &hsi->clients, next) {
        if (hci->hc == hc) { 
            found = true;
            break;
        }
    }

    if (!found) {
        return len;
    }

    for (int i=0; i<len; i+=INPUT_SIZE){
        switch (data[i+0]) {
            case HARMONY_SERVER_INPUT_MULTITOUCH:
                multitouch_event(hci, read_u8(data, i+1), read_u16(data, i+2), read_u16(data, i+4));
                break;
            case HARMONY_SERVER_INPUT_KEY:
                key_event(hci, read_u8(data, i+1), read_u32(data, i+2));
                break;
            default:
                break;
        }
    }

    return len;
}

static void harmony_server_input_disconnect(HarmonyClient *hc, void *opaque) 
{
    HarmonyServerInput *hsi = opaque;
    HarmonyClientInput *hci;
    bool found = false;

    QTAILQ_FOREACH(hci, &hsi->clients, next) {
        if (hci->hc == hc) { 
            found = true;
            break;
        }
    }

    if (found) {
        QTAILQ_REMOVE(&hsi->clients, hci, next);
        g_free(hci);
    }
}

void harmony_server_input_init(HarmonyServerDisplay *hsd, Error **errp)
{
    HarmonyServerInput *hsi = g_new0(HarmonyServerInput, 1);
    QTAILQ_INIT(&hsi->clients);
    hsi->hsd = hsd;
    hsi->last_tracking_id = -1;

    if (keyboard_layout) {
        hsi->kbd_layout = init_keyboard_layout(name2keysym,
                                              keyboard_layout, errp);
    } else {
        hsi->kbd_layout = init_keyboard_layout(name2keysym, "en-us", errp);
    }

    if (!hsi->kbd_layout) {
        return;
    }

    hsi->kbd = qkbd_state_init(hsd->dcl.con);

    harmony_server_add_path_handler("/input", 
                                harmony_server_input_connect, 
                                harmony_server_input_read, 
                                harmony_server_input_disconnect, 
                                hsi);
}
