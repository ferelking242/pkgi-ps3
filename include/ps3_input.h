#pragma once

/*
 * PKGi Remastered — robust PS3 input layer.
 *
 * Fixes third-party / non-genuine controller issues by:
 *  - scanning every connected port instead of hard-coding port 0;
 *  - using libpad's normalized padData fields instead of assuming a
 *    particular raw report layout or frame marker;
 *  - keeping the last known-good state when no valid frame arrives
 *    (no dead buttons, no ghost inputs, UI never runs unattended);
 *  - adding analog-stick hysteresis so a noisy stick cannot flicker
 *    navigation on and off around a single cutoff;
 *  - exposing edge detection (pressed/released), held state, and a
 *    time-based controlled repeat for navigation.
 *
 * Reported buttons use the PKGI_BUTTON_* masks consumed by the existing UI.
 */

#include <stdint.h>

#define PS3IN_MAX_PORTS 7

/* UI-facing input events, hardware independent. */
typedef enum {
    UI_INPUT_NONE = 0,
    UI_INPUT_UP,
    UI_INPUT_DOWN,
    UI_INPUT_LEFT,
    UI_INPUT_RIGHT,
    UI_INPUT_ACCEPT,    /* X or O, depending on system settings */
    UI_INPUT_CANCEL,    /* O or X */
    UI_INPUT_MENU,      /* Triangle */
    UI_INPUT_SECONDARY, /* Square */
    UI_INPUT_OPTIONS,   /* START */
    UI_INPUT_INFO,      /* SELECT */
    UI_INPUT_PAGE_PREV, /* L1 / L2 */
    UI_INPUT_PAGE_NEXT  /* R1 / R2 */
} UiInput;

typedef struct {
    uint32_t pressed;   /* edge: down this frame, not last frame */
    uint32_t released;  /* edge: up this frame, down last frame */
    uint32_t held;      /* currently down (hysteresis-applied for stick) */
    UiInput  event;     /* single action/navigation event for this frame */
    uint32_t pad_connected;
} pkgi_ui_input;

void ps3in_init(void);
void ps3in_set_buttons(uint32_t ok_button, uint32_t cancel_button);
void ps3in_poll(uint64_t delta_us, pkgi_ui_input* out);
int  ps3in_connected(void);
