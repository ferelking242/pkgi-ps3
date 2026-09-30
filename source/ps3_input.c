#include "ps3_input.h"

#include <io/pad.h>
#include <string.h>

/*
 * PKGi Remastered input layer.
 *
 * Fixes third-party / non-genuine controller issues:
 *  - scans every connected port (port 0 was hard-coded before);
 *  - validates each frame (len > 0 and the libpad 0x7 marker, with the
 *    alternate packing some pads use) before trusting it;
 *  - keeps the last known-good logical state when no valid frame shows
 *    up, emitting no new edges (no dead buttons, no ghost inputs);
 *  - analog stick hysteresis (engage/release thresholds) instead of a
 *    single flickering cutoff;
 *  - time-based repeat for navigation instead of frame-count repeat.
 *
 * Button bits reuse the proven PKGI_BUTTON_* values (ya2d reads the
 * same libpad data), so DualShock 3 behavior is unchanged.
 */

/* Analog hysteresis thresholds on raw axis bytes (center 0x80). */
#define ANALOG_ON  0x4A
#define ANALOG_OFF 0x60

/* Time-based key repeat (microseconds). */
#define REPEAT_DELAY_US   (350 * 1000)
#define REPEAT_PERIOD_US  (110 * 1000)

typedef struct {
    uint32_t down;       /* hysteresis-applied logical state */
    uint32_t prev_down;  /* previous frame logical state */
    int      latch_ly;   /* 0 none, -1 up, +1 down */
    int      latch_lx;   /* 0 none, -1 left, +1 right */
    uint64_t repeat_timer;
    int      repeating;
    int      connected;
} input_state;

static input_state in;
static uint32_t g_ok_button;
static uint32_t g_cancel_button;

void ps3in_set_buttons(uint32_t ok_button, uint32_t cancel_button)
{
    g_ok_button = ok_button;
    g_cancel_button = cancel_button;
}

int ps3in_connected(void)
{
    return in.connected;
}

static void ps3in_frame(const padData* d, int* valid, int* swapped,
                        uint32_t* buttons, uint16_t* lx, uint16_t* ly)
{
    if (d->len <= 0)
        return;

    uint16_t b0 = d->button[0];
    uint16_t b1 = d->button[1];

    /* libpad sets the 0x7 frame marker for every valid read. Genuine
     * pads put it in the high nibble of button[1]; some third-party
     * pads land it elsewhere, so accept the alternate packing too. */
    int marker_std = ((b1 & 0xF000) == 0x7000);
    int marker_alt = ((b0 & 0x00F0) == 0x0070);

    if (!marker_std && !marker_alt)
        return;

    uint8_t bytes[4];
    memcpy(bytes, &d->button[2], sizeof(uint32_t));
    uint32_t v;
    if (marker_std)
    {
        v = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
            ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
    }
    else
    {
        /* Alternate packing: the two layout bytes shifted by two. */
        v = (uint32_t)bytes[2] | ((uint32_t)bytes[3] << 8) |
            ((uint32_t)bytes[0] << 16) | ((uint32_t)bytes[1] << 24);
    }

    *valid = 1;
    *swapped = marker_alt && !marker_std;
    *buttons = v;
    *lx = d->ANA_L_H & 0xFF;
    *ly = d->ANA_L_V & 0xFF;
}

void ps3in_init(void)
{
    ioPadInit(PS3IN_MAX_PORTS);
    memset(&in, 0, sizeof(in));
}

static int scan_ports(padData* out_data)
{
    padInfo2 info;
    memset(&info, 0, sizeof(info));

    if (ioPadGetInfo2(&info) != 0)
        return 0;

    for (int i = 0; i < PS3IN_MAX_PORTS; i++)
    {
        if (!info.port_status[i])
            continue;

        memset(out_data, 0, sizeof(padData));
        if (ioPadGetData(i, out_data) != 0)
            continue;

        int valid = 0, swapped = 0;
        uint32_t buttons = 0;
        uint16_t lx, ly;
        ps3in_frame(out_data, &valid, &swapped, &buttons, &lx, &ly);
        if (valid)
            return 1;
    }

    return 0;
}

static void apply_axis(uint16_t v, int* latch, uint32_t* nav,
                       uint32_t low_bit, uint32_t high_bit)
{
    if (*latch == 0)
    {
        if (v < ANALOG_ON)
        {
            *latch = -1;
            *nav |= low_bit;
        }
        else if (v > 0xFF - ANALOG_ON)
        {
            *latch = 1;
            *nav |= high_bit;
        }
    }
    else if (*latch < 0)
    {
        if (v >= ANALOG_OFF)
            *latch = 0;
        else
            *nav |= low_bit;
    }
    else
    {
        if (v <= 0xFF - ANALOG_OFF)
            *latch = 0;
        else
            *nav |= high_bit;
    }
}

void ps3in_poll(uint64_t delta_us, pkgi_ui_input* out)
{
    padData data;
    memset(&data, 0, sizeof(data));

    int found = scan_ports(&data);
    memset(out, 0, sizeof(*out));

    if (!found)
    {
        /* No valid frame: freeze state, no edges (UI cannot run away). */
        in.connected = 0;
        out->pad_connected = 0;
        out->held = in.down;
        return;
    }

    in.connected = 1;
    out->pad_connected = 1;

    int valid = 0, swapped = 0;
    uint32_t raw = 0;
    uint16_t lx = 0x80, ly = 0x80;
    ps3in_frame(&data, &valid, &swapped, &raw, &lx, &ly);

    /* D-pad bits pass through directly. */
    uint32_t nav = raw & (PS3IN_UP | PS3IN_DOWN | PS3IN_LEFT | PS3IN_RIGHT);

    /* Analog stick with hysteresis. */
    apply_axis(ly, &in.latch_ly, &nav, PS3IN_UP, PS3IN_DOWN);
    apply_axis(lx, &in.latch_lx, &nav, PS3IN_LEFT, PS3IN_RIGHT);

    in.prev_down = in.down;
    in.down = nav;

    out->pressed  = in.down & ~in.prev_down;
    out->released = in.prev_down & ~in.down;
    out->held     = in.down;

    /* Navigation repeat: directions only, time-based, one event/frame. */
    uint32_t nav_mask = PS3IN_UP | PS3IN_DOWN | PS3IN_LEFT | PS3IN_RIGHT;
    uint32_t nav_now = in.down & nav_mask;

    if (nav_now && (nav_now == (in.prev_down & nav_mask)))
    {
        in.repeat_timer += delta_us;
        if (!in.repeating && in.repeat_timer >= REPEAT_DELAY_US)
            in.repeating = 1;
        if (in.repeating && in.repeat_timer >= REPEAT_PERIOD_US)
        {
            in.repeat_timer -= REPEAT_PERIOD_US;
            if (nav_now & PS3IN_UP)         out->event = UI_INPUT_UP;
            else if (nav_now & PS3IN_DOWN)  out->event = UI_INPUT_DOWN;
            else if (nav_now & PS3IN_LEFT)  out->event = UI_INPUT_LEFT;
            else if (nav_now & PS3IN_RIGHT) out->event = UI_INPUT_RIGHT;
        }
    }
    else
    {
        in.repeat_timer = 0;
        in.repeating = 0;
    }

    /* Fresh presses win over repeats, one action per frame. */
    if (out->pressed)
    {
        uint32_t p = out->pressed;
        if (p & PS3IN_UP)                        out->event = UI_INPUT_UP;
        else if (p & PS3IN_DOWN)                 out->event = UI_INPUT_DOWN;
        else if (p & PS3IN_LEFT)                 out->event = UI_INPUT_LEFT;
        else if (p & PS3IN_RIGHT)                out->event = UI_INPUT_RIGHT;
        else if (p & g_ok_button)                out->event = UI_INPUT_ACCEPT;
        else if (p & g_cancel_button)            out->event = UI_INPUT_CANCEL;
        else if (p & PS3IN_TRIANGLE)             out->event = UI_INPUT_MENU;
        else if (p & PS3IN_SQUARE)               out->event = UI_INPUT_SECONDARY;
        else if (p & PS3IN_START)                out->event = UI_INPUT_OPTIONS;
        else if (p & PS3IN_SELECT)               out->event = UI_INPUT_INFO;
        else if (p & (PS3IN_L1 | PS3IN_L2))      out->event = UI_INPUT_PAGE_PREV;
        else if (p & (PS3IN_R1 | PS3IN_R2))      out->event = UI_INPUT_PAGE_NEXT;
    }
}
