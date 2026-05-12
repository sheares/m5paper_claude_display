#pragma once
#include <stddef.h>

// Callback fired when a complete response has been received over BLE.
typedef void (*nus_response_cb_t)(const char *text);

// Initialise BLE NUS server. Advertises as "M5Paper-Claude".
// cb is called from the BLE task — copy text before returning.
void ble_nus_init(nus_response_cb_t cb);

// Send a short string to the connected central (button choice reply).
void ble_nus_send(const char *text);

// True if a central is currently connected.
bool ble_nus_connected(void);

// Latest STAT line received from daemon (empty string until first STAT).
const char *ble_nus_metrics(void);

// True if a STAT packet arrived since the last call to this function.
// Used by main loop to trigger an idle redraw on stats change.
bool ble_nus_metrics_changed(void);

#define BLE_MAX_CHOICES 5
#define BLE_CHOICE_MAXLEN 32

// Latest choice labels. *n_out is set to active count (0..BLE_MAX_CHOICES).
// Returns pointer to internal storage; do not modify.
const char (*ble_nus_choices(int *n_out))[BLE_CHOICE_MAXLEN];
