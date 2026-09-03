#ifndef INPUT_HANDLER_H
#define INPUT_HANDLER_H

#include "qemu/queue.h"
#include "ui/kbd-state.h"
#include "ui/keymaps.h"
#include "harmony-server.h"
#include "display-handler.h"

typedef struct HarmonyServerInput HarmonyServerInput;
typedef struct HarmonyClientInput HarmonyClientInput;

struct HarmonyServerInput {
    HarmonyServerDisplay *hsd;

    QTAILQ_HEAD(, HarmonyClientInput) clients;

    int last_tracking_id;
    int tracking_ids[8];

    kbd_layout_t *kbd_layout;
    QKbdState *kbd;
};

struct HarmonyClientInput {
    HarmonyServerInput *hsi;
    HarmonyClient *hc;
    QTAILQ_ENTRY(HarmonyClientInput) next;

    uint8_t last_bmask;

    bool multitouch;
    Notifier mouse_mode_notifier;
};

extern void harmony_server_input_init(HarmonyServerDisplay *hsd, Error **errp);

#endif // INPUT_HANDLER_H
