#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_common_api.h"
#include "ble_nus.h"
#include "bm8563.h"

#define TAG "ble_nus"

// Nordic UART Service UUIDs
#define NUS_SERVICE_UUID  0x6E400001, 0xB5A3, 0xF393, 0xE0A9, 0xE50E24DCCA9E
#define NUS_RX_UUID       0x6E400002, 0xB5A3, 0xF393, 0xE0A9, 0xE50E24DCCA9E
#define NUS_TX_UUID       0x6E400003, 0xB5A3, 0xF393, 0xE0A9, 0xE50E24DCCA9E

// Full 128-bit UUIDs as byte arrays (little-endian)
static const uint8_t NUS_SVC_UUID128[16]  = {
    0x9E,0xCA,0xDC,0x24,0x0E,0xE5,0xA9,0xE0,
    0x93,0xF3,0xA3,0xB5,0x01,0x00,0x40,0x6E
};
static const uint8_t NUS_RX_UUID128[16] = {
    0x9E,0xCA,0xDC,0x24,0x0E,0xE5,0xA9,0xE0,
    0x93,0xF3,0xA3,0xB5,0x02,0x00,0x40,0x6E
};
static const uint8_t NUS_TX_UUID128[16] = {
    0x9E,0xCA,0xDC,0x24,0x0E,0xE5,0xA9,0xE0,
    0x93,0xF3,0xA3,0xB5,0x03,0x00,0x40,0x6E
};

// GATT attribute indices
enum {
    IDX_SVC,
    IDX_RX_CHAR,
    IDX_RX_VAL,
    IDX_TX_CHAR,
    IDX_TX_VAL,
    IDX_TX_CCCD,
    IDX_MAX
};

#define RX_BUF_MAX  16384   // max accumulated response size

static uint16_t s_handle[IDX_MAX];
static uint16_t s_conn_id      = 0xFFFF;
static uint16_t s_gatts_if     = ESP_GATT_IF_NONE;
static bool     s_notify_en    = false;
static nus_response_cb_t s_cb  = NULL;

// Receive buffer — accumulates chunked text
static char    s_rxbuf[RX_BUF_MAX];
static size_t  s_rxlen = 0;

// Latest STAT line + change flag
static char    s_metrics[96] = {0};
static bool    s_metrics_changed = false;

// Latest choice labels (default ACK)
static char    s_choices[BLE_MAX_CHOICES][BLE_CHOICE_MAXLEN] = { "ACK" };
static int     s_n_choices = 1;

// ── advertising ──────────────────────────────────────────────────────────────

static uint8_t s_adv_data[] = {
    0x02, 0x01, 0x06,           // Flags: LE General Discoverable, no BR/EDR
    0x11, 0x07,                 // Complete 128-bit service UUID (17 bytes)
    0x9E,0xCA,0xDC,0x24,0x0E,0xE5,0xA9,0xE0,
    0x93,0xF3,0xA3,0xB5,0x01,0x00,0x40,0x6E,
};

static uint8_t s_scan_rsp[] = {
    0x0F, 0x09,  // Length=15, Type=Complete Local Name
    'M','5','P','a','p','e','r','-','C','l','a','u','d','e'
};

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min       = 0x20,
    .adv_int_max       = 0x40,
    .adv_type          = ADV_TYPE_IND,
    .own_addr_type     = BLE_ADDR_TYPE_PUBLIC,
    .channel_map       = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void start_advertising(void)
{
    esp_ble_gap_config_adv_data_raw(s_adv_data, sizeof(s_adv_data));
    esp_ble_gap_config_scan_rsp_data_raw(s_scan_rsp, sizeof(s_scan_rsp));
}

// ── GATT helpers — must be declared before s_attr_tab ────────────────────────

static const uint16_t primary_service_uuid        = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t character_declaration_uuid  = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t character_client_config_uuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
static const uint8_t  char_prop_write  = ESP_GATT_CHAR_PROP_BIT_WRITE |
                                          ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
static const uint8_t  char_prop_notify = ESP_GATT_CHAR_PROP_BIT_NOTIFY;

// ── GATT service table ────────────────────────────────────────────────────────

static const esp_gatts_attr_db_t s_attr_tab[] = {
    // Service declaration
    [IDX_SVC] = {
        { ESP_GATT_AUTO_RSP },
        { ESP_UUID_LEN_16, (uint8_t *)&primary_service_uuid, ESP_GATT_PERM_READ,
          sizeof(NUS_SVC_UUID128), sizeof(NUS_SVC_UUID128), (uint8_t *)NUS_SVC_UUID128 }
    },
    // RX characteristic declaration
    [IDX_RX_CHAR] = {
        { ESP_GATT_AUTO_RSP },
        { ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid,
          ESP_GATT_PERM_READ, 1, 1, (uint8_t *)&char_prop_write }
    },
    // RX characteristic value (central writes here to send text to device)
    [IDX_RX_VAL] = {
        { ESP_GATT_AUTO_RSP },
        { ESP_UUID_LEN_128, (uint8_t *)NUS_RX_UUID128,
          ESP_GATT_PERM_WRITE, 512, 0, NULL }
    },
    // TX characteristic declaration
    [IDX_TX_CHAR] = {
        { ESP_GATT_AUTO_RSP },
        { ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid,
          ESP_GATT_PERM_READ, 1, 1, (uint8_t *)&char_prop_notify }
    },
    // TX characteristic value (device notifies central with button choices)
    [IDX_TX_VAL] = {
        { ESP_GATT_AUTO_RSP },
        { ESP_UUID_LEN_128, (uint8_t *)NUS_TX_UUID128,
          ESP_GATT_PERM_READ, 512, 0, NULL }
    },
    // TX CCCD (central writes 0x0001 to enable notifications)
    [IDX_TX_CCCD] = {
        { ESP_GATT_AUTO_RSP },
        { ESP_UUID_LEN_16, (uint8_t *)&character_client_config_uuid,
          ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, 2, 0, NULL }
    },
};

// ── receive framing ───────────────────────────────────────────────────────────
// Protocol (Mac → M5Paper over BLE NUS RX):
//   First packet:  "LEN:<decimal_length>\n"  — announces total byte count
//   Data packets:  raw text chunks
//   Final packet:  "\x04"  (EOT — signals end of transmission)

static uint32_t s_expected_len = 0;

static void handle_rx(const uint8_t *data, uint16_t len)
{
    // Check for EOT
    if (len == 1 && data[0] == 0x04) {
        s_rxbuf[s_rxlen] = '\0';
        ESP_LOGI(TAG, "RX complete: %u bytes", (unsigned)s_rxlen);
        if (s_cb && s_rxlen > 0) s_cb(s_rxbuf);
        s_rxlen = 0;
        s_expected_len = 0;
        return;
    }

    // CHCS:Label1|Label2|...  — set button labels for the next response
    if (len > 5 && memcmp(data, "CHCS:", 5) == 0) {
        char tmp[BLE_MAX_CHOICES * (BLE_CHOICE_MAXLEN + 1) + 1] = {0};
        size_t n = len - 5 < sizeof(tmp) - 1 ? len - 5 : sizeof(tmp) - 1;
        memcpy(tmp, data + 5, n);
        while (n > 0 && (tmp[n-1] == '\n' || tmp[n-1] == '\r')) tmp[--n] = '\0';

        int count = 0;
        char *save = NULL;
        for (char *tok = strtok_r(tmp, "|", &save);
             tok && count < BLE_MAX_CHOICES;
             tok = strtok_r(NULL, "|", &save)) {
            strncpy(s_choices[count], tok, BLE_CHOICE_MAXLEN - 1);
            s_choices[count][BLE_CHOICE_MAXLEN - 1] = '\0';
            count++;
        }
        if (count == 0) {
            strcpy(s_choices[0], "ACK");
            count = 1;
        }
        s_n_choices = count;
        ESP_LOGI(TAG, "CHCS: %d labels", s_n_choices);
        return;
    }

    // STAT:<text>  — preformatted status line for the top bar
    if (len > 5 && memcmp(data, "STAT:", 5) == 0) {
        size_t n = len - 5 < sizeof(s_metrics) - 1 ? len - 5 : sizeof(s_metrics) - 1;
        memcpy(s_metrics, data + 5, n);
        // Strip trailing newline
        while (n > 0 && (s_metrics[n-1] == '\n' || s_metrics[n-1] == '\r')) n--;
        s_metrics[n] = '\0';
        s_metrics_changed = true;
        ESP_LOGI(TAG, "STAT: %s", s_metrics);
        return;
    }

    // TIME:Y,M,D,W,H,M,S  — one-shot RTC set
    if (len > 5 && memcmp(data, "TIME:", 5) == 0) {
        char tmp[64] = {0};
        size_t n = len - 5 < sizeof(tmp) - 1 ? len - 5 : sizeof(tmp) - 1;
        memcpy(tmp, data + 5, n);
        bm8563_time_t t = {0};
        int y, mo, d, w, h, mi, s;
        if (sscanf(tmp, "%d,%d,%d,%d,%d,%d,%d", &y, &mo, &d, &w, &h, &mi, &s) == 7) {
            t.year = y; t.month = mo; t.day = d; t.weekday = w;
            t.hour = h; t.minute = mi; t.second = s;
            bm8563_set(&t);
            ESP_LOGI(TAG, "RTC set: %04d-%02d-%02d %02d:%02d:%02d wd=%d",
                     y, mo, d, h, mi, s, w);
        } else {
            ESP_LOGW(TAG, "bad TIME packet: %s", tmp);
        }
        return;
    }

    // Check for LEN header
    if (len > 4 && memcmp(data, "LEN:", 4) == 0) {
        s_expected_len = (uint32_t)atoi((const char *)data + 4);
        s_rxlen = 0;
        ESP_LOGI(TAG, "Expecting %u bytes", (unsigned)s_expected_len);
        return;
    }

    // Accumulate data
    if (s_rxlen + len < RX_BUF_MAX) {
        memcpy(s_rxbuf + s_rxlen, data, len);
        s_rxlen += len;
    } else {
        ESP_LOGW(TAG, "RX buffer overflow, truncating");
    }
}

// ── GATTS event handler ───────────────────────────────────────────────────────

static void gatts_event_handler(esp_gatts_cb_event_t event,
                                 esp_gatt_if_t gatts_if,
                                 esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTS_REG_EVT:
        s_gatts_if = gatts_if;
        start_advertising();
        esp_ble_gatts_create_attr_tab(s_attr_tab, gatts_if, IDX_MAX, 0);
        break;

    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if (param->add_attr_tab.status == ESP_GATT_OK &&
            param->add_attr_tab.num_handle == IDX_MAX) {
            memcpy(s_handle, param->add_attr_tab.handles,
                   IDX_MAX * sizeof(uint16_t));
            esp_ble_gatts_start_service(s_handle[IDX_SVC]);
        }
        break;

    case ESP_GATTS_MTU_EVT:
        ESP_LOGI(TAG, "MTU negotiated: %d", param->mtu.mtu);
        break;

    case ESP_GATTS_CONNECT_EVT:
        s_conn_id = param->connect.conn_id;
        s_notify_en = false;
        s_rxlen = 0;
        esp_ble_conn_update_params_t conn_params = {
            .min_int = 0x10, .max_int = 0x20,
            .latency = 0, .timeout = 400,
        };
        memcpy(conn_params.bda, param->connect.remote_bda, 6);
        esp_ble_gap_update_conn_params(&conn_params);
        // Request large MTU
        esp_ble_gatt_set_local_mtu(517);
        ESP_LOGI(TAG, "connected");
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        s_conn_id   = 0xFFFF;
        s_notify_en = false;
        esp_ble_gap_start_advertising(&s_adv_params);
        ESP_LOGI(TAG, "disconnected, re-advertising");
        break;

    case ESP_GATTS_WRITE_EVT:
        if (param->write.handle == s_handle[IDX_RX_VAL]) {
            handle_rx(param->write.value, param->write.len);
        } else if (param->write.handle == s_handle[IDX_TX_CCCD]) {
            s_notify_en = (param->write.value[0] == 0x01);
        }
        break;

    default:
        break;
    }
}

static void gap_event_handler(esp_gap_ble_cb_event_t event,
                               esp_ble_gap_cb_param_t *param)
{
    if (event == ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT ||
        event == ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT) {
        esp_ble_gap_start_advertising(&s_adv_params);
    }
}

// ── public API ────────────────────────────────────────────────────────────────

void ble_nus_init(nus_response_cb_t cb)
{
    s_cb = cb;

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(0));
    ESP_ERROR_CHECK(esp_ble_gatt_set_local_mtu(517));

    esp_ble_gap_set_device_name("M5Paper-Claude");
}

void ble_nus_send(const char *text)
{
    if (!s_notify_en || s_conn_id == 0xFFFF) return;
    esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id, s_handle[IDX_TX_VAL],
                                (uint16_t)strlen(text), (uint8_t *)text, false);
}

bool ble_nus_connected(void)
{
    return s_conn_id != 0xFFFF;
}

const char *ble_nus_metrics(void)
{
    return s_metrics;
}

bool ble_nus_metrics_changed(void)
{
    bool changed = s_metrics_changed;
    s_metrics_changed = false;
    return changed;
}

const char (*ble_nus_choices(int *n_out))[BLE_CHOICE_MAXLEN]
{
    if (n_out) *n_out = s_n_choices;
    return (const char (*)[BLE_CHOICE_MAXLEN])s_choices;
}
