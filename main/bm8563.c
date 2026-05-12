#include "driver/i2c.h"
#include "esp_log.h"
#include "bm8563.h"

#define TAG "bm8563"

#define I2C_PORT  I2C_NUM_0
#define BM8563_ADDR 0x51

static uint8_t bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t bin2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

static esp_err_t reg_read(uint8_t reg, uint8_t *buf, size_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BM8563_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BM8563_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, buf, len, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t r = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return r;
}

static esp_err_t reg_write(uint8_t reg, const uint8_t *buf, size_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BM8563_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write(cmd, buf, len, true);
    i2c_master_stop(cmd);
    esp_err_t r = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return r;
}

bool bm8563_init(void)
{
    // Clear stop and test bits in control register 1
    uint8_t ctrl[2] = {0x00, 0x00};
    if (reg_write(0x00, ctrl, 2) != ESP_OK) {
        ESP_LOGE(TAG, "BM8563 write failed");
        return false;
    }
    return true;
}

bool bm8563_read(bm8563_time_t *t)
{
    uint8_t buf[7];
    if (reg_read(0x02, buf, 7) != ESP_OK) return false;

    t->second  = bcd2bin(buf[0] & 0x7F);
    t->minute  = bcd2bin(buf[1] & 0x7F);
    t->hour    = bcd2bin(buf[2] & 0x3F);
    t->day     = bcd2bin(buf[3] & 0x3F);
    t->weekday = buf[4] & 0x07;
    t->month   = bcd2bin(buf[5] & 0x1F);
    t->year    = bcd2bin(buf[6]) + ((buf[5] & 0x80) ? 1900 : 2000);
    return true;
}

bool bm8563_set(const bm8563_time_t *t)
{
    uint8_t buf[7];
    buf[0] = bin2bcd(t->second);
    buf[1] = bin2bcd(t->minute);
    buf[2] = bin2bcd(t->hour);
    buf[3] = bin2bcd(t->day);
    buf[4] = t->weekday & 0x07;
    buf[5] = bin2bcd(t->month) | (t->year < 2000 ? 0x80 : 0x00);
    buf[6] = bin2bcd(t->year % 100);
    return reg_write(0x02, buf, 7) == ESP_OK;
}
