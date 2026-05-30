#pragma once

#include "esp_err.h"

#define SDCARD_ROOT "/sdcard"

esp_err_t sdcard_init(void);
void      sdcard_deinit(void);
