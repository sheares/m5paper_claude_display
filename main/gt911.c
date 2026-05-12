#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "gt911.h"

#define TAG "gt911"

#define I2C_PORT    I2C_NUM_0
#define I2C_SDA     21
#define I2C_SCL     22
#define TOUCH_INT   36      // input-only GPIO, interrupt (not used in polling mode)
#define GT911_ADDR  0x5D    // M5Paper: INT held low at reset → address 0x5D (I2C scan confirmed)

// Register map
#define REG_CMD         0x8040
#define REG_STATUS      0x814E
#define REG_TOUCH_BASE  0x8150

static esp_err_t i2c_write(uint16_t reg, const uint8_t *data, size_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (GT911_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, (reg >> 8) & 0xFF, true);
    i2c_master_write_byte(cmd, reg & 0xFF, true);
    if (data && len) i2c_master_write(cmd, data, len, true);
    i2c_master_stop(cmd);
    esp_err_t r = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return r;
}

static esp_err_t i2c_read(uint16_t reg, uint8_t *buf, size_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (GT911_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, (reg >> 8) & 0xFF, true);
    i2c_master_write_byte(cmd, reg & 0xFF, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (GT911_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, buf, len, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t r = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return r;
}

bool gt911_init(void)
{
    i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = I2C_SDA,
        .scl_io_num       = I2C_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &cfg));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0));

    // Scan for any I2C devices
    ESP_LOGI(TAG, "I2C scan:");
    for (uint8_t addr = 1; addr < 127; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t r = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(10));
        i2c_cmd_link_delete(cmd);
        if (r == ESP_OK) {
            ESP_LOGI(TAG, "  found device at 0x%02X", addr);
        }
    }

    // No soft-reset write — factory config is already loaded in chip NVM.
    // Just put the chip into coordinate-read mode.
    uint8_t cmd = 0x00;
    i2c_write(REG_CMD, &cmd, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Verify chip ID
    uint8_t id[4] = {0};
    if (i2c_read(0x8140, id, 4) != ESP_OK) {
        ESP_LOGE(TAG, "GT911 not found at 0x%02X", GT911_ADDR);
        return false;
    }
    ESP_LOGI(TAG, "GT911 ID: %c%c%c%c", id[0], id[1], id[2], id[3]);

    // Clear status flag — otherwise stale buf_ready from previous firmware
    // can leave us reading 0x80,n=0 forever.
    uint8_t zero = 0;
    i2c_write(REG_STATUS, &zero, 1);
    return true;
}

bool gt911_read(gt911_point_t *p)
{
    uint8_t status = 0;
    if (i2c_read(REG_STATUS, &status, 1) != ESP_OK) return false;

    uint8_t n_touches = status & 0x0F;
    bool    buf_ready = (status & 0x80) != 0;

    if (!buf_ready || n_touches == 0) {
        // Clear stuck buf_ready flag even on no-touch so chip can refresh.
        if (buf_ready) { uint8_t zero = 0; i2c_write(REG_STATUS, &zero, 1); }
        return false;
    }

    // REG_TOUCH_BASE (0x8150) is X1_LSB directly.
    // Layout per point: X_LSB, X_MSB, Y_LSB, Y_MSB, SIZE_LSB, SIZE_MSB.
    uint8_t tp[4];
    esp_err_t r = i2c_read(REG_TOUCH_BASE, tp, 4);

    // Clear status AFTER reading coords so chip refills.
    uint8_t zero = 0;
    i2c_write(REG_STATUS, &zero, 1);

    if (r != ESP_OK) return false;

    p->x = (uint16_t)tp[0] | ((uint16_t)tp[1] << 8);
    p->y = (uint16_t)tp[2] | ((uint16_t)tp[3] << 8);
    // GT911 reports portrait-native: x in [0..539], y in [0..959].
    return true;
}
