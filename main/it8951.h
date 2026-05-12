#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Physical panel dimensions (landscape hardware — used by IT8951 driver)
#define PANEL_W     960
#define PANEL_H     540

// Logical UI dimensions (portrait orientation — used by drawing code)
#define EPD_WIDTH   540
#define EPD_HEIGHT  960

// Update modes
#define IT8951_UPD_INIT  0   // full clear (slow)
#define IT8951_UPD_DU    1   // direct update 1-bit (fast)
#define IT8951_UPD_GC16  2   // high quality 16-gray (slow, ~2s)
#define IT8951_UPD_A2    6   // fast 1-bit, may ghost

typedef struct {
    uint16_t width;
    uint16_t height;
    uint32_t img_buf_addr;
} it8951_info_t;

// Call once at startup; populates info
esp_err_t it8951_init(it8951_info_t *info);

// Load a 1bpp framebuffer into IT8951 and refresh the display.
// buf: EPD_WIDTH/8 * EPD_HEIGHT bytes, MSB = leftmost pixel, 1=white 0=black
// mode: IT8951_UPD_* constant
void it8951_display(const it8951_info_t *info, const uint8_t *buf, uint8_t mode);

// Refresh only a sub-rectangle of the panel. x,y,w,h are in landscape
// coordinates (panel-native: 0..959 horizontal, 0..539 vertical). The fb
// must still be the full 960×540 1bpp landscape framebuffer. x and w should
// be multiples of 4 (4bpp word alignment) — the helper will pad if not.
void it8951_display_area(const it8951_info_t *info, const uint8_t *fb,
                         uint16_t x, uint16_t y,
                         uint16_t w, uint16_t h, uint8_t mode);

// Full white clear
void it8951_clear(const it8951_info_t *info);
