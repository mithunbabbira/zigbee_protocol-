#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t led_controller_init(void);
void led_controller_set(bool on);
void led_controller_blink(uint32_t duration_ms);

#ifdef __cplusplus
}
#endif
