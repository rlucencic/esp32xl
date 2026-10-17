#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "xl.h"

/* Machine settings kept in NVS (namespace "atarixl") */
typedef struct {
    uint8_t model;          /* xl_model_t */
    uint8_t tv;             /* xl_tv_t */
    uint8_t basic;          /* 1: BASIC on */
    uint8_t joystick;       /* port the keypad joystick is in: 1 or 2 */
} settings_t;

extern settings_t settings;

void settings_load(void);
void settings_save(void);
