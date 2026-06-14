#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t zigbee_coordinator_init(void);
void zigbee_coordinator_start_task(void);
void zigbee_coordinator_open_network(uint16_t duration_sec);
void zigbee_coordinator_send_on_off(const uint8_t ieee[8], bool on);
void zigbee_coordinator_send_blink(const uint8_t ieee[8], uint32_t duration_ms);

#ifdef __cplusplus
}
#endif
