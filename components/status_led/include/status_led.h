#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t status_led_init(void);
void status_led_set(bool on);

#ifdef __cplusplus
}
#endif
