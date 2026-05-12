#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "it8951.h"
#include "gt911.h"
#include "bm8563.h"
#include "ui.h"
#include "ble_nus.h"

#define TAG "main"

// GPIO 2 must stay HIGH or the board loses power
#define MAIN_PWR_PIN 2

// Side buttons (active LOW, external pullups on M5Paper)
#define BTN_LEFT_PIN   37
#define BTN_MID_PIN    38
#define BTN_RIGHT_PIN  39

// Return to idle clock after this many seconds with no interaction
#define SLEEP_AFTER_S 600

typedef enum { STATE_IDLE, STATE_RESPONSE, STATE_SENDING } state_t;

static QueueHandle_t s_response_q;   // heap char* sent from BLE task
static it8951_info_t s_epd;
static uint8_t      *s_fb;
static state_t       s_state = STATE_IDLE;
static int64_t       s_last_response_us;
static int           s_highlight = 0;    // currently highlighted button index
static char         *s_last_response_text = NULL;  // kept for redraw-on-cycle

// ── BLE callback (runs in BLE task — must be quick) ───────────────────────────

static void on_ble_response(const char *text)
{
    char *copy = malloc(strlen(text) + 1);
    if (!copy) return;
    strcpy(copy, text);
    if (xQueueSend(s_response_q, &copy, pdMS_TO_TICKS(200)) != pdTRUE)
        free(copy);
}

// ── display helpers ───────────────────────────────────────────────────────────

static bm8563_time_t get_time(void)
{
    bm8563_time_t t = {0};
    bm8563_read(&t);
    return t;
}

static void show_idle(void)
{
    bm8563_time_t t = get_time();
    const char *conn    = ble_nus_connected() ? "BLE" : "...";
    const char *metrics = ble_nus_metrics();
    ui_draw_idle(s_fb, &t, metrics, conn);
    it8951_display(&s_epd, s_fb, IT8951_UPD_GC16);
    s_state = STATE_IDLE;
}

static void show_response(const char *text)
{
    bm8563_time_t t = get_time();
    const char *conn    = ble_nus_connected() ? "BLE" : "---";
    const char *metrics = ble_nus_metrics();
    int n = 0;
    const char (*labels)[32] = ble_nus_choices(&n);
    s_highlight = 0;   // reset highlight on each new response

    // Cache text for cycle-redraw
    free(s_last_response_text);
    s_last_response_text = strdup(text);

    ui_draw_response(s_fb, text, &t, metrics, conn, labels, n, s_highlight, -1);
    // DU mode: 1-bit, ~260ms refresh, low ghosting (cleaner than A2).
    it8951_display(&s_epd, s_fb, IT8951_UPD_DU);
    s_state = STATE_RESPONSE;
    s_last_response_us = esp_timer_get_time();
}

// Flash the just-pressed button in inverted colors, partial-area A2 refresh.
// Provides instant tap feedback while the BLE notify is sent.
static void flash_pressed(int idx)
{
    if (!s_last_response_text) return;
    bm8563_time_t t = get_time();
    const char *conn    = ble_nus_connected() ? "BLE" : "---";
    const char *metrics = ble_nus_metrics();
    int n = 0;
    const char (*labels)[32] = ble_nus_choices(&n);

    ui_draw_response(s_fb, s_last_response_text, &t, metrics, conn,
                     labels, n, s_highlight, idx);

    int btn_y_p = 0, btn_h_p = 0;
    ui_btn_area(labels, n, &btn_y_p, &btn_h_p);
    it8951_display_area(&s_epd, s_fb,
                        (uint16_t)btn_y_p, 0,
                        (uint16_t)btn_h_p, (uint16_t)EPD_WIDTH,
                        IT8951_UPD_A2);
}

// Redraw only the button strip after a highlight change. Much faster than
// a full-panel refresh — touches ~210×540 pixels instead of 540×960.
static void redraw_highlight(void)
{
    if (!s_last_response_text) return;
    bm8563_time_t t = get_time();
    const char *conn    = ble_nus_connected() ? "BLE" : "---";
    const char *metrics = ble_nus_metrics();
    int n = 0;
    const char (*labels)[32] = ble_nus_choices(&n);

    // Rebuild full framebuffer (in-memory; cheap)
    ui_draw_response(s_fb, s_last_response_text, &t, metrics, conn,
                     labels, n, s_highlight, -1);

    // Push only the button area to the panel.
    // Portrait button area (0, btn_y_p, EPD_WIDTH, btn_h_p)
    // → landscape (lx = btn_y_p, ly = 0, lw = btn_h_p, lh = EPD_WIDTH)
    int btn_y_p = 0, btn_h_p = 0;
    ui_btn_area(labels, n, &btn_y_p, &btn_h_p);
    it8951_display_area(&s_epd, s_fb,
                        (uint16_t)btn_y_p, 0,
                        (uint16_t)btn_h_p, (uint16_t)EPD_WIDTH,
                        IT8951_UPD_A2);   // A2 ~150ms; brief highlight, ghost OK
}

static void show_sending(const char *choice)
{
    ui_draw_sending(s_fb, choice);
    it8951_display(&s_epd, s_fb, IT8951_UPD_DU);
    s_state = STATE_SENDING;
}

// ── main ──────────────────────────────────────────────────────────────────────

void app_main(void)
{
    // Hold main power (must be first)
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << MAIN_PWR_PIN),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(MAIN_PWR_PIN, 1);

    // NVS (required by BLE)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Framebuffer — prefer PSRAM so internal RAM stays free for BT stack
    s_fb = heap_caps_malloc(FB_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_fb) s_fb = malloc(FB_SIZE);
    configASSERT(s_fb);
    ui_init(s_fb);

    // E-paper display
    ESP_ERROR_CHECK(it8951_init(&s_epd));

    // I2C peripherals (GT911 touch + BM8563 RTC share one bus)
    gt911_init();
    bm8563_init();

    // Side buttons (input only, rely on external pullups on M5Paper)
    gpio_config_t btn_io = {
        .pin_bit_mask = (1ULL << BTN_LEFT_PIN) |
                         (1ULL << BTN_MID_PIN)  |
                         (1ULL << BTN_RIGHT_PIN),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&btn_io);

    // BLE NUS
    s_response_q = xQueueCreate(4, sizeof(char *));
    ble_nus_init(on_ble_response);

    // Initial screen: idle clock + "..." until BLE connects
    ESP_LOGI(TAG, "calling show_idle");
    show_idle();
    ESP_LOGI(TAG, "show_idle done");

    gt911_point_t touch;
    int64_t clock_tick = esp_timer_get_time();

    while (1) {
        // New response arrived from BLE
        char *incoming = NULL;
        if (xQueueReceive(s_response_q, &incoming, 0) == pdTRUE) {
            if (strlen(incoming) == 0) {
                show_idle();
            } else {
                show_response(incoming);
            }
            free(incoming);
        }

        int n_choices = 0;
        const char (*choice_labels)[32] = ble_nus_choices(&n_choices);

        // Touch events
        if (gt911_read(&touch)) {
            int idx = ui_hit_button_idx(touch.x, touch.y, choice_labels, n_choices);
            if (idx >= 0 && s_state == STATE_RESPONSE) {
                ESP_LOGI(TAG, "tap %s (idx=%d x=%d y=%d)",
                         choice_labels[idx], idx, touch.x, touch.y);
                flash_pressed(idx);
                ble_nus_send(choice_labels[idx]);
                vTaskDelay(pdMS_TO_TICKS(400));
                show_idle();
            }
        }

        // Side button edge detection (pressed = LOW)
        static int prev_l = 1, prev_m = 1, prev_r = 1;
        int cur_l = gpio_get_level(BTN_LEFT_PIN);
        int cur_m = gpio_get_level(BTN_MID_PIN);
        int cur_r = gpio_get_level(BTN_RIGHT_PIN);

        if (s_state == STATE_RESPONSE && n_choices > 0) {
            if (prev_l == 1 && cur_l == 0) {
                s_highlight = (s_highlight + n_choices - 1) % n_choices;
                ESP_LOGI(TAG, "LEFT → highlight %s", choice_labels[s_highlight]);
                redraw_highlight();
            }
            if (prev_r == 1 && cur_r == 0) {
                s_highlight = (s_highlight + 1) % n_choices;
                ESP_LOGI(TAG, "RIGHT → highlight %s", choice_labels[s_highlight]);
                redraw_highlight();
            }
            if (prev_m == 1 && cur_m == 0) {
                const char *label = choice_labels[s_highlight];
                ESP_LOGI(TAG, "MID → confirm %s", label);
                flash_pressed(s_highlight);
                ble_nus_send(label);
                vTaskDelay(pdMS_TO_TICKS(400));
                show_idle();
            }
        }
        prev_l = cur_l; prev_m = cur_m; prev_r = cur_r;

        // Sleep to idle after inactivity
        if (s_state == STATE_RESPONSE) {
            int64_t elapsed = (esp_timer_get_time() - s_last_response_us) / 1000000;
            if (elapsed >= SLEEP_AFTER_S) show_idle();
        }

        // Refresh idle clock every 60 s, or immediately when stats updated
        if (s_state == STATE_IDLE) {
            bool tick = (esp_timer_get_time() - clock_tick) >= 60000000LL;
            bool stat = ble_nus_metrics_changed();
            if (tick || stat) {
                clock_tick = esp_timer_get_time();
                show_idle();
            }
        } else {
            ble_nus_metrics_changed();    // consume flag so it doesn't fire on next idle
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
