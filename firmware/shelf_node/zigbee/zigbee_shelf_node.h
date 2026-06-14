#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t zigbee_shelf_node_init(void);
void zigbee_shelf_node_start_task(void);
void zigbee_shelf_node_on_button_pressed(void);

#ifdef __cplusplus
}
#endif
