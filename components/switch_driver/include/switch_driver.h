#pragma once

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *switch_driver_handle_t;

#define SWITCH_INV_HANDLE NULL

typedef void (*switch_event_cb_t)(switch_driver_handle_t handle);

typedef struct {
    gpio_num_t gpio_num;
    switch_event_cb_t event_cb;
} switch_driver_config_t;

switch_driver_handle_t switch_driver_init(const switch_driver_config_t *config);
void switch_driver_deinit(switch_driver_handle_t handle);

#ifdef __cplusplus
}
#endif
