#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint16_t x;
    uint16_t y;
} gt911_point_t;

// Returns true if a touch event was read, fills *p with first touch point.
bool gt911_init(void);
bool gt911_read(gt911_point_t *p);
