/* Controller-only save-dialog input, shared SDL2/XInput snapshot and repeat.
 * No game state, windows, or save files are read/written here. */
#ifndef PS3RECOMP_SAVEDLG_INPUT_H
#define PS3RECOMP_SAVEDLG_INPUT_H
#include <stdint.h>
#include "../input/pad_test.h"

enum { SDI_UP = 1, SDI_DOWN = 2, SDI_OK = 4, SDI_CANCEL = 8 };
typedef struct { unsigned previous, blocked; uint32_t repeat_at; int ready, devices; } savedlg_input;
typedef struct { int move, confirm, cancel; } savedlg_action;

static unsigned savedlg_pad_buttons(const PadTestInfo* info, int* devices)
{
    unsigned buttons = 0;
    *devices = 0;
    for (int i = 0; i < info->ndev && i < PAD_TEST_MAX_DEVICES; i++) {
        const PadTestState* state = &info->dev[i].st;
        if (!state->connected) continue;
        ++*devices;
        /* Already mapped CELL_PAD_CTRL bits, before keyboard/focus filtering. */
        if ((state->buttons & (1u << 4)) || state->ly < 64) buttons |= SDI_UP;
        if ((state->buttons & (1u << 6)) || state->ly > 192) buttons |= SDI_DOWN;
        if (state->buttons & (1u << 13)) buttons |= SDI_OK;
        if (state->buttons & (1u << 14)) buttons |= SDI_CANCEL;
    }
    return buttons;
}

static savedlg_action savedlg_input_step(savedlg_input* input, unsigned buttons, int devices, int focused, uint32_t now)
{
    savedlg_action action = {0, 0, 0};
    if (!focused) { input->ready = 0; return action; }
    if (!input->ready || devices != input->devices) {
        input->ready = 1; input->devices = devices; input->blocked = buttons; input->previous = 0;
        return action;
    }
    input->blocked &= buttons; /* opening/hot-plug/focus-held buttons need release */
    buttons &= ~input->blocked;
    unsigned pressed = buttons & ~input->previous;
    unsigned dirs = buttons & (SDI_UP | SDI_DOWN);
    if (pressed & (SDI_UP | SDI_DOWN)) {
        action.move = (pressed & SDI_UP) ? -1 : 1;
        input->repeat_at = now + 400;
    } else if (dirs && (int32_t)(now - input->repeat_at) >= 0) {
        action.move = (dirs & SDI_UP) ? -1 : 1;
        input->repeat_at = now + 110;
    }
    action.confirm = !!(pressed & SDI_OK);
    action.cancel = !action.confirm && !!(pressed & SDI_CANCEL);
    input->previous = buttons;
    return action;
}
#endif
