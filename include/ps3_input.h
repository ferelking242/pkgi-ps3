#pragma once

/*
 * PKGi Remastered — robust PS3 input layer.
 *
 * Fixes third-party / non-genuine controller issues by:
 *  - scanning every connected port instead of hard-coding port 0;
 *  - validating each padData frame (len > 0 plus the 0x7 frame marker)
 *    before trusting its contents;
 *  - keeping the last known-good state when no valid frame arrives
 *    (no dead buttons, no ghost inputs, UI never runs unattended);
 *  - adding analog-stick hysteresis so a noisy stick cannot flicker
 *    navigation on and off around a single cutoff;
 *  - exposing edge detection (pressed/released), held state, and a
 *    time-based controlled repeat for navigation.
 *
 * Button bits reuse the existing PKGI_BUTTON_* values, which are proven
 * to work with genuine DualShock pads through the ya2d/libpad path.
 */

#include <stdint.h>

#define PS3IN_MAX_PORTS 7

/* Hardware button bits (same values as PKGI_BUTTON_*). */
#define PS3IN_SELECT (1u << 16)
#define PS3IN_L3     (1u << 1)
#define PS3IN_R3     (1u << 2)
#define PS3IN_START  (1u << 3)
#define PS3IN_UP     (1u << 4)
#define PS3IN_RIGHT  (1u << 5)
#define PS3IN_DOWN   (1u << 6)
#define PS3IN_LEFT   (1u << 7)

#define PS3IN_L2     (1u << 0)
#define PS3IN_R2     (1u << 1)
#define PS3IN_L1     (1u << 2)
#define PS3IN_R1     (1u << 3)
#define PS3IN_TRIANGLE (1u << 4)
#define PS3IN_CIRCLE   (1u << 5)
#define PS3IN_CROSS    (1u << 6)
#define PS3IN_SQUARE   (1u << 7)

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
