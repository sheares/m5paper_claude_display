#include <string.h>
#include <stdio.h>
#include "ui.h"
#include "font.h"
#include "font_big.h"

// ── pixel helpers ─────────────────────────────────────────────────────────────

static inline void set_px(uint8_t *fb, int x, int y, int white)
{
    // Portrait (x,y) → landscape (lx,ly): 90° CW rotation
    // portrait x [0..539] → landscape y [539..0] (inverted)
    // portrait y [0..959] → landscape x [0..959]
    int lx = y;
    int ly = EPD_WIDTH - 1 - x;
    if (lx < 0 || lx >= PANEL_W || ly < 0 || ly >= PANEL_H) return;
    uint8_t *b = &fb[ly * FB_STRIDE + lx / 8];
    uint8_t  m = 1 << (7 - (lx & 7));
    if (white) *b |= m; else *b &= ~m;
}

static void fill_rect(uint8_t *fb, int x, int y, int w, int h, int white)
{
    for (int row = y; row < y + h; row++) {
        for (int col = x; col < x + w; col++) {
            set_px(fb, col, row, white);
        }
    }
}

static void draw_hline(uint8_t *fb, int x, int y, int w, int white)
{
    for (int i = x; i < x + w; i++) set_px(fb, i, y, white);
}

// ── scaled glyph ──────────────────────────────────────────────────────────────

// Draw one character at (x,y), font scale S.
// bg=0 means black-on-white; bg=1 means white-on-black.
static void draw_char(uint8_t *fb, int x, int y, char c, int scale, int bg)
{
    if (c < FONT_FIRST || c > FONT_LAST) c = '?';
    const uint8_t *g = font5x8[(uint8_t)c - FONT_FIRST];
    for (int col = 0; col < FONT_W; col++) {
        for (int row = 0; row < FONT_H; row++) {
            int ink = (g[col] >> row) & 1;
            if (!bg) ink = !ink;
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++)
                    set_px(fb, x + col*scale + sx, y + row*scale + sy, ink);
        }
    }
}

static void draw_str(uint8_t *fb, int x, int y, const char *s, int scale, int bg)
{
    int stride = FONT_W * scale + 1;
    while (*s) {
        draw_char(fb, x, y, *s++, scale, bg);
        x += stride;
    }
}

// 16x32 native bitmap font (Menlo) — used for body text + buttons.
// scale parameter is reserved (1 = native, larger = pixel-doubled).
static void draw_char_big(uint8_t *fb, int x, int y, char c, int bg)
{
    if (c < FONTB_FIRST || c > FONTB_LAST) c = '?';
    const uint16_t *g = font_big[(uint8_t)c - FONTB_FIRST];
    for (int row = 0; row < FONTB_H; row++) {
        uint16_t bits = g[row];
        for (int col = 0; col < FONTB_W; col++) {
            int ink = (bits >> (FONTB_W - 1 - col)) & 1;
            if (!bg) ink = !ink;
            set_px(fb, x + col, y + row, ink);
        }
    }
}

static void draw_str_big(uint8_t *fb, int x, int y, const char *s, int bg)
{
    while (*s) {
        draw_char_big(fb, x, y, *s++, bg);
        x += FONTB_W;
    }
}

static void draw_str_big_centered(uint8_t *fb, int rx, int rw, int y,
                                   const char *s, int bg)
{
    int len = (int)strlen(s);
    int tw  = len * FONTB_W;
    int ox  = rx + (rw - tw) / 2;
    draw_str_big(fb, ox, y, s, bg);
}

// Draw string centered in [rx, rx+rw)
static void draw_str_centered(uint8_t *fb, int rx, int rw, int y,
                               const char *s, int scale, int bg)
{
    int len    = (int)strlen(s);
    int stride = FONT_W * scale + 1;
    int tw     = len * stride - 1;
    int ox     = rx + (rw - tw) / 2;
    draw_str(fb, ox, y, s, scale, bg);
}

// ── word wrap ─────────────────────────────────────────────────────────────────

// Fills lines[0..n-1] with pointers into src (not null-terminated; use lens[]).
// Returns number of lines produced.
static int wrap_text(const char *src, char lines[MAX_LINES][BODY_CPL + 1])
{
    int n = 0;
    while (*src && n < MAX_LINES) {
        // Skip leading spaces except after a newline
        while (*src == ' ') src++;
        if (!*src) break;

        // Handle explicit newlines: blank line → skip
        if (*src == '\n') { src++; continue; }

        const char *line_start = src;
        int len = 0;
        const char *last_space = NULL;
        int last_space_len = 0;

        while (*src && *src != '\n' && len < BODY_CPL) {
            if (*src == ' ') { last_space = src; last_space_len = len; }
            src++;
            len++;
        }

        int used = len;
        if (*src && *src != '\n' && last_space) {
            // Break at last space
            used = last_space_len;
            src  = last_space + 1;
        } else if (*src == '\n') {
            src++;
        }

        memcpy(lines[n], line_start, used);
        lines[n][used] = '\0';
        n++;
    }
    return n;
}

// ── layout sections ───────────────────────────────────────────────────────────

static void draw_divider(uint8_t *fb, int y)
{
    draw_hline(fb, 0, y, EPD_WIDTH, 0);
    draw_hline(fb, 0, y+1, EPD_WIDTH, 0);
}

// Big-font labels use native 16x32. If the label is too wide for the cell,
// fall back to the 5x8 font at a scale that fits.
static bool fits_big(const char *label, int btn_w)
{
    return (int)strlen(label) * FONTB_W + 8 <= btn_w;
}

static int label_scale_for(const char *label, int btn_w)
{
    int len = (int)strlen(label);
    if (len <= 0) return 4;
    for (int s = 4; s >= 1; s--) {
        int tw = len * (FONT_W * s + 1) - 1;
        if (tw + 8 <= btn_w) return s;
    }
    return 1;
}

// Layout decision: stack vertically when any label is "long" or count is high.
typedef struct {
    bool stacked;
    int  y;            // top of button area
    int  area_h;       // total height of button area
    int  cell_w;       // width of each button
    int  cell_h;       // height of each button
    int  gap;          // gap between buttons (narrow padding)
} btn_layout_t;

static bool labels_are_long(const char (*labels)[32], int n)
{
    if (n >= 4) return true;
    for (int i = 0; i < n; i++) {
        if ((int)strlen(labels[i]) > 6) return true;
    }
    return false;
}

static void compute_btn_layout(const char (*labels)[32], int n, btn_layout_t *L)
{
    if (n <= 0) {
        L->stacked = false; L->gap = 2;
        L->y = BTN_Y; L->area_h = BTN_H;
        L->cell_w = EPD_WIDTH; L->cell_h = BTN_H;
        return;
    }
    L->stacked = labels_are_long(labels, n);
    if (L->stacked) {
        L->gap    = 6;            // wider gap between stacked buttons
        L->cell_h = 48;
        L->cell_w = EPD_WIDTH;
        L->area_h = n * L->cell_h + (n - 1) * L->gap;
        L->y      = EPD_HEIGHT - L->area_h;
    } else {
        L->gap     = 2;
        L->y       = BTN_Y;
        L->area_h  = BTN_H;
        L->cell_h  = BTN_H;
        L->cell_w  = (EPD_WIDTH - L->gap * (n - 1)) / n;
    }
}

// Draw thick border inside given rect, color opposite of fill.
static void draw_rect_highlight(uint8_t *fb, int x, int y, int w, int h, int fill)
{
    int border = !fill;
    const int thick = 7;
    for (int i = 0; i < thick; i++) {
        for (int xx = x + i; xx < x + w - i; xx++) {
            set_px(fb, xx, y + i, border);
            set_px(fb, xx, y + h - 1 - i, border);
        }
        for (int yy = y + i; yy < y + h - i; yy++) {
            set_px(fb, x + i, yy, border);
            set_px(fb, x + w - 1 - i, yy, border);
        }
    }
}

static void draw_buttons(uint8_t *fb, const btn_layout_t *L,
                         const char (*labels)[32], int n, int hl, int pressed)
{
    if (n <= 0) return;

    for (int i = 0; i < n; i++) {
        int cx, cy, cw, ch;
        if (L->stacked) {
            cx = 0;
            cy = L->y + i * (L->cell_h + L->gap);
            cw = L->cell_w;
            ch = L->cell_h;
        } else {
            cx = i * (L->cell_w + L->gap);
            cy = L->y;
            // last cell absorbs rounding remainder
            cw = (i == n - 1) ? (EPD_WIDTH - cx) : L->cell_w;
            ch = L->cell_h;
        }

        int pressed_here = (pressed == i);
        int fill = pressed_here ? 0 : 1;        // pressed → black bg
        int text = pressed_here ? 1 : 0;        // pressed → white text
        fill_rect(fb, cx, cy, cw, ch, fill);

        if (fits_big(labels[i], cw)) {
            // Visible glyph occupies rows ~5-25 of the 32-row block; shift
            // up so the visible portion sits in the cell's vertical centre.
            int ty = cy + (ch - FONTB_H) / 2 - 3;
            draw_str_big_centered(fb, cx, cw, ty, labels[i], text);
        } else {
            int s  = label_scale_for(labels[i], cw);
            int ty = cy + (ch - FONT_H * s) / 2;
            draw_str_centered(fb, cx, cw, ty, labels[i], s, text);
        }

        // 1px outline (inverse of fill so it's always visible)
        int outline = !fill;
        for (int xx = cx; xx < cx + cw; xx++) {
            set_px(fb, xx, cy, outline);
            set_px(fb, xx, cy + ch - 1, outline);
        }
        for (int yy = cy; yy < cy + ch; yy++) {
            set_px(fb, cx, yy, outline);
            set_px(fb, cx + cw - 1, yy, outline);
        }

        if (hl == i && !pressed_here) draw_rect_highlight(fb, cx, cy, cw, ch, 1);

        // separator line on the side opposite the gap
        if (L->stacked) {
            if (i < n - 1) {
                int sy = cy + ch;
                for (int x = 0; x < EPD_WIDTH; x++) set_px(fb, x, sy, 0);
            }
        } else {
            if (i < n - 1) {
                int sx = cx + cw;
                for (int y = cy; y < cy + ch; y++) {
                    set_px(fb, sx, y, 0);
                    set_px(fb, sx + 1, y, 0);
                }
            }
        }
    }
}

// ── public API ────────────────────────────────────────────────────────────────

void ui_init(uint8_t *fb)
{
    memset(fb, 0xFF, FB_SIZE);   // all white
}

void ui_clear(uint8_t *fb)
{
    memset(fb, 0xFF, FB_SIZE);
}

void ui_draw_status(uint8_t *fb, const bm8563_time_t *t,
                    const char *metrics, const char *conn)
{
    fill_rect(fb, 0, 0, EPD_WIDTH, STATUS_H, 1);
    int sty = (STATUS_H - FONT_H * SCALE_STATUS) / 2;

    // Left: HH:MM
    if (t) {
        char hm[8];
        snprintf(hm, sizeof(hm), "%02d:%02d", t->hour, t->minute);
        draw_str(fb, 8, sty, hm, SCALE_STATUS, 0);
    }

    // Right: connection indicator
    int conn_w = 0;
    if (conn && *conn) {
        conn_w = (int)strlen(conn) * (FONT_W * SCALE_STATUS + 1) - 1;
        draw_str(fb, EPD_WIDTH - conn_w - 8, sty, conn, SCALE_STATUS, 0);
    }

    // Center: metrics — centered between time (~70px wide) and conn block
    if (metrics && *metrics) {
        int mw = (int)strlen(metrics) * (FONT_W * SCALE_STATUS + 1) - 1;
        int left_edge  = 70;
        int right_edge = EPD_WIDTH - (conn_w ? conn_w + 16 : 8);
        int span       = right_edge - left_edge;
        int mx         = left_edge + (span - mw) / 2;
        if (mx < left_edge) mx = left_edge;   // overflow-left guard
        draw_str(fb, mx, sty, metrics, SCALE_STATUS, 0);
    }

    draw_divider(fb, STATUS_H);
}

void ui_draw_idle(uint8_t *fb, const bm8563_time_t *t,
                  const char *metrics, const char *conn)
{
    ui_clear(fb);
    ui_draw_status(fb, t, metrics, conn);

    // Large centered clock
    char clk[8] = "--:--";
    if (t) snprintf(clk, sizeof(clk), "%02d:%02d", t->hour, t->minute);

    int scale = 8;
    int cw = (int)strlen(clk) * (FONT_W * scale + 1) - 1;
    int ch = FONT_H * scale;
    int cx = (EPD_WIDTH  - cw) / 2;
    int cy = TEXT_Y + (TEXT_H - ch) / 2 - 40;
    draw_str(fb, cx, cy, clk, scale, 0);

    // Date subtitle below clock
    if (t) {
        const char *days[]   = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
        const char *months[] = {"","Jan","Feb","Mar","Apr","May","Jun",
                                 "Jul","Aug","Sep","Oct","Nov","Dec"};
        char date[32];
        snprintf(date, sizeof(date), "%s  %d %s %d",
                 days[t->weekday % 7], t->day,
                 months[t->month < 13 ? t->month : 0], t->year);
        int dscale = 3;
        int dw = (int)strlen(date) * (FONT_W * dscale + 1) - 1;
        int dx = (EPD_WIDTH - dw) / 2;
        int dy = cy + ch + 32;
        draw_str(fb, dx, dy, date, dscale, 0);
    }
}

void ui_draw_response(uint8_t *fb, const char *text,
                      const bm8563_time_t *t,
                      const char *metrics, const char *conn,
                      const char (*labels)[32], int n_labels,
                      int highlight_idx, int pressed_idx)
{
    ui_clear(fb);
    ui_draw_status(fb, t, metrics, conn);

    btn_layout_t L;
    compute_btn_layout(labels, n_labels, &L);

    int text_area_h = L.y - TEXT_Y - 2;
    int max_rows    = text_area_h / BODY_LH;

    // Wrap body text
    static char lines[MAX_LINES][BODY_CPL + 1];
    int n = wrap_text(text, lines);

    int visible = n < max_rows ? n : max_rows;
    for (int i = 0; i < visible; i++) {
        int lx = 8;
        int ly = TEXT_Y + 4 + i * BODY_LH;
        draw_str_big(fb, lx, ly, lines[i], 0);
    }
    if (n > max_rows) {
        draw_str_big(fb, 8, TEXT_Y + text_area_h - BODY_LH, "...", 0);
    }

    draw_divider(fb, L.y);
    draw_buttons(fb, &L, labels, n_labels, highlight_idx, pressed_idx);
}

void ui_draw_sending(uint8_t *fb, const char *choice)
{
    ui_clear(fb);

    char msg[32];
    snprintf(msg, sizeof(msg), "Sending: %s", choice);

    int scale = 4;
    int cw = (int)strlen(msg) * (FONT_W * scale + 1) - 1;
    int ch = FONT_H * scale;
    int cx = (EPD_WIDTH  - cw) / 2;
    int cy = (EPD_HEIGHT - ch) / 2;
    draw_str(fb, cx, cy, msg, scale, 0);
}

void ui_btn_area(const char (*labels)[32], int n_labels,
                 int *out_y, int *out_h)
{
    btn_layout_t L;
    compute_btn_layout(labels, n_labels, &L);
    if (out_y) *out_y = L.y;
    if (out_h) *out_h = L.area_h;
}

int ui_hit_button_idx(int x, int y,
                      const char (*labels)[32], int n_labels)
{
    if (n_labels <= 0) return -1;
    if (x < 0 || x >= EPD_WIDTH) return -1;

    btn_layout_t L;
    compute_btn_layout(labels, n_labels, &L);

    if (L.stacked) {
        if (y < L.y || y >= L.y + L.area_h) return -1;
        int idx = (y - L.y) / (L.cell_h + L.gap);
        if (idx >= n_labels) idx = n_labels - 1;
        return idx;
    }

    if (y < L.y || y >= L.y + L.cell_h) return -1;
    int idx = x / (L.cell_w + L.gap);
    if (idx >= n_labels) idx = n_labels - 1;
    return idx;
}
