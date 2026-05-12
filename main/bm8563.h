#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t hour;    // 0-23
    uint8_t minute;  // 0-59
    uint8_t second;  // 0-59
    uint8_t day;     // 1-31
    uint8_t month;   // 1-12
    uint8_t weekday; // 0=Sun 6=Sat
    uint16_t year;   // e.g. 2026
} bm8563_time_t;

bool bm8563_init(void);   // call after gt911_init (shares I2C bus)
bool bm8563_read(bm8563_time_t *t);
bool bm8563_set(const bm8563_time_t *t);
