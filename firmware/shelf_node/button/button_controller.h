#pragma once

#include "esp_err.h"
#include "switch_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*button_controller_cb_t)(void);

esp_err_t button_controller_init(button_controller_cb_t cb);

#ifdef __cplusplus
}
#endif
