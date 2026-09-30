#include "ps3_input.h"
#include "pkgi.h"

#include <io/pad.h>
#include <string.h>

/*
 * PKGi Remastered input layer.
 *
 * Fixes third-party / non-genuine controller issues:
 *  - scans every connected port (port 0 was hard-coded before);
 *  - reads libpad's normalized padData bitfields rather than guessing the
 *    raw controller report layout or requiring one marker packing;
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
static padData g_pad_cache[PS3IN_MAX_PORTS];
static uint8_t g_pad_cache_valid[PS3IN_MAX_PORTS];

void ps3in_set_buttons(uint32_t ok_button, uint32_t cancel_button)
{
    g_ok_button = ok_button;
    g_cancel_button = cancel_button;
}

int ps3in_connected(void)
{
    return in.connected;
}

static int ps3in_frame(const padData* d, uint32_t* buttons,
                       uint16_t* lx, uint16_t* ly)
{
    if (d->len <= 0)
        return 0;

    /*
     * PSL1GHT/libpad already normalizes controller reports into these
     * padData bitfields. Reading button[] bytes and requiring a particular
     * 0x7 marker rejects valid third-party pads and does not match PKGi's
     * legacy button masks.
     */
    uint32_t value = 0;
    if (d->BTN_SELECT)  value |= PKGI_BUTTON_SELECT;
    if (d->BTN_START)   value |= PKGI_BUTTON_START;
    if (d->BTN_UP)      value |= PKGI_BUTTON_UP;
    if (d->BTN_RIGHT)   value |= PKGI_BUTTON_RIGHT;
    if (d->BTN_DOWN)    value |= PKGI_BUTTON_DOWN;
    if (d->BTN_LEFT)    value |= PKGI_BUTTON_LEFT;
    if (d->BTN_L1)      value |= PKGI_BUTTON_LT;
    if (d->BTN_R1)      value |= PKGI_BUTTON_RT;
    if (d->BTN_L2)      value |= PKGI_BUTTON_L2;
    if (d->BTN_R2)      value |= PKGI_BUTTON_R2;
    if (d->BTN_TRIANGLE) value |= PKGI_BUTTON_T;
    if (d->BTN_CIRCLE)  value |= PKGI_BUTTON_O;
    if (d->BTN_CROSS)   value |= PKGI_BUTTON_X;
    if (d->BTN_SQUARE)  value |= PKGI_BUTTON_S;

    *buttons = value;
    *lx = (uint16_t)d->ANA_L_H;
    *ly = (uint16_t)d->ANA_L_V;
    return 1;
}

static int axis_is_active(uint16_t value)
{
    return value < ANALOG_ON || value > 0xFF - ANALOG_ON;
}

void ps3in_init(void)
{
    /* ya2d_init() already initializes libpad; do not initialize it twice. */
    memset(&in, 0, sizeof(in));
    memset(g_pad_cache, 0, sizeof(g_pad_cache));
    memset(g_pad_cache_valid, 0, sizeof(g_pad_cache_valid));
}

static int scan_ports(padData* out_data)
{
    padInfo2 info;
    padData first_neutral;
    int have_neutral = 0;
    memset(&info, 0, sizeof(info));

    if (ioPadGetInfo2(&info) != 0)
        return 0;

    for (int i = 0; i < PS3IN_MAX_PORTS; i++)
    {
        if (!info.port_status[i])
        {
            g_pad_cache_valid[i] = 0;
            continue;
        }

        /*
         * PSL1GHT only fills padData when that port's input changes; a
         * zero-length read is not a neutral controller state. Keep the last
         * sample so held buttons, analog position, and key repeat persist.
         */
        padData changed;
        memset(&changed, 0, sizeof(changed));
        if (ioPadGetData(i, &changed) == 0 && changed.len > 0)
        {
            g_pad_cache[i] = changed;
            g_pad_cache_valid[i] = 1;
        }

        if (!g_pad_cache_valid[i])
            continue;

        uint32_t buttons = 0;
        uint16_t lx = 0x80, ly = 0x80;
        if (!ps3in_frame(&g_pad_cache[i], &buttons, &lx, &ly))
            continue;

        if (!have_neutral)
        {
            first_neutral = g_pad_cache[i];
            have_neutral = 1;
        }

        /*
         * Prefer a pad that is actually being used over a connected but
         * idle pad on a lower-numbered port. If several are active, port
         * order makes the choice deterministic.
         */
        if (buttons || axis_is_active(lx) || axis_is_active(ly))
        {
            *out_data = g_pad_cache[i];
            return 1;
        }
    }

    if (!have_neutral)
        return 0;

    *out_data = first_neutral;
    return 1;
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

    uint32_t raw = 0;
    uint16_t lx = 0x80, ly = 0x80;
    if (!found || !ps3in_frame(&data, &raw, &lx, &ly))
    {
        /* No valid frame: freeze state, no edges (UI cannot run away). */
        in.connected = 0;
        out->pad_connected = 0;
        out->held = in.down;
        return;
    }

    in.connected = 1;
    out->pad_connected = 1;

    const uint32_t nav_mask = PKGI_BUTTON_UP | PKGI_BUTTON_DOWN |
                              PKGI_BUTTON_LEFT | PKGI_BUTTON_RIGHT;
    uint32_t nav = raw & nav_mask;

    /* Analog stick with hysteresis. */
    apply_axis(ly, &in.latch_ly, &nav, PKGI_BUTTON_UP, PKGI_BUTTON_DOWN);
    apply_axis(lx, &in.latch_lx, &nav, PKGI_BUTTON_LEFT, PKGI_BUTTON_RIGHT);

    in.prev_down = in.down;
    /* Preserve action buttons as well as navigation in the held state. */
    in.down = (raw & ~nav_mask) | nav;

    out->pressed  = in.down & ~in.prev_down;
    out->released = in.prev_down & ~in.down;
    out->held     = in.down;

    /* Navigation repeat: directions only, time-based, one event/frame. */
    uint32_t nav_now = in.down & nav_mask;

    if (nav_now && (nav_now == (in.prev_down & nav_mask)))
    {
        in.repeat_timer += delta_us;
        if (!in.repeating && in.repeat_timer >= REPEAT_DELAY_US)
            in.repeating = 1;
        if (in.repeating && in.repeat_timer >= REPEAT_PERIOD_US)
        {
            in.repeat_timer -= REPEAT_PERIOD_US;
            if (nav_now & PKGI_BUTTON_UP)         out->event = UI_INPUT_UP;
            else if (nav_now & PKGI_BUTTON_DOWN)  out->event = UI_INPUT_DOWN;
            else if (nav_now & PKGI_BUTTON_LEFT)  out->event = UI_INPUT_LEFT;
            else if (nav_now & PKGI_BUTTON_RIGHT) out->event = UI_INPUT_RIGHT;
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
        if (p & PKGI_BUTTON_UP)                  out->event = UI_INPUT_UP;
        else if (p & PKGI_BUTTON_DOWN)           out->event = UI_INPUT_DOWN;
        else if (p & PKGI_BUTTON_LEFT)           out->event = UI_INPUT_LEFT;
        else if (p & PKGI_BUTTON_RIGHT)          out->event = UI_INPUT_RIGHT;
        else if (p & g_ok_button)                out->event = UI_INPUT_ACCEPT;
        else if (p & g_cancel_button)            out->event = UI_INPUT_CANCEL;
        else if (p & PKGI_BUTTON_T)              out->event = UI_INPUT_MENU;
        else if (p & PKGI_BUTTON_S)              out->event = UI_INPUT_SECONDARY;
        else if (p & PKGI_BUTTON_START)          out->event = UI_INPUT_OPTIONS;
        else if (p & PKGI_BUTTON_SELECT)         out->event = UI_INPUT_INFO;
        else if (p & (PKGI_BUTTON_LT | PKGI_BUTTON_L2))
            out->event = UI_INPUT_PAGE_PREV;
        else if (p & (PKGI_BUTTON_RT | PKGI_BUTTON_R2))
            out->event = UI_INPUT_PAGE_NEXT;
    }
}
