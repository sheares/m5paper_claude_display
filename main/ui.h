#pragma once
#include <stdint.h>
#include "it8951.h"
#include "bm8563.h"

// Physical framebuffer: 960×540 landscape (matches IT8951 hardware)
// set_px() rotates portrait UI coordinates into this landscape buffer.
#define FB_STRIDE  (PANEL_W / 8)           // 120 bytes per row (physical)
#define FB_SIZE    (FB_STRIDE * PANEL_H)   // 64800 bytes total

// Layout constants in portrait space (EPD_WIDTH=540, EPD_HEIGHT=960)
#define STATUS_H    40      // status bar height
#define TEXT_Y      42      // text area top (below status + divider)
#define TEXT_H      810     // text area height (960 - 42 - 108)
#define BTN_Y       852     // button row top
#define BTN_H       108     // button row height
#define YES_W       200     // YES button width
#define ABCD_W      85      // each A/B/C/D button  (200 + 4×85 = 540)

// Font scale factor for status bar (still uses 5x8 because space is tight)
#define SCALE_STATUS  2     // 10×16px chars for status bar

// Body + button text use the native 16x32 bitmap (font_big.h)
#define BODY_CW    16
#define BODY_CH    32
#define BODY_LH    28      // tighter than full height — glyphs use ~22 rows
#define BODY_CPL   (EPD_WIDTH / BODY_CW)   // 33 chars per line
#define BODY_ROWS  (TEXT_H / BODY_LH)      // ~28 visible rows

// Maximum lines of wrapped body text
#define MAX_LINES  128

void ui_init(uint8_t *fb);
void ui_clear(uint8_t *fb);
void ui_draw_status(uint8_t *fb, const bm8563_time_t *t,
                    const char *metrics, const char *conn);
void ui_draw_idle(uint8_t *fb, const bm8563_time_t *t,
                  const char *metrics, const char *conn);
// Labels: array of [32]-wide strings, count = active count.
// pressed_idx: if >= 0, that button is rendered inverted (instant tap feedback).
void ui_draw_response(uint8_t *fb, const char *text,
                      const bm8563_time_t *t,
                      const char *metrics, const char *conn,
                      const char (*labels)[32], int n_labels,
                      int highlight_idx, int pressed_idx);
void ui_draw_sending(uint8_t *fb, const char *choice);

// Returns button index (0..n_labels-1) or -1 if no hit.
// Labels are needed because the layout (horizontal vs stacked) depends on them.
int ui_hit_button_idx(int x, int y,
                      const char (*labels)[32], int n_labels);

// Reports the button area's top-y and height in portrait coords.
// Used by callers that want to refresh only that region.
void ui_btn_area(const char (*labels)[32], int n_labels,
                 int *out_y, int *out_h);
