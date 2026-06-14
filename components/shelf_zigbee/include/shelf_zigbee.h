/*
 * Shared shelf Zigbee stack memory tuning.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SHELF_ZIGBEE_STORAGE_PARTITION_NAME "zb_storage"

#define SHELF_NETWORK_SIZE       (256U)
#define SHELF_BUFFER_POOL_ZC     (200U)
#define SHELF_BUFFER_POOL_ZR     (160U)
#define SHELF_BUFFER_POOL_ZED    (80U)
#define SHELF_BIND_TABLE_SIZE    (32U)
#define SHELF_MAX_CHILDREN_ZC    (50U)
#define SHELF_MAX_CHILDREN_ZR    (20U)

esp_err_t shelf_zigbee_apply_coordinator_memory(void);
esp_err_t shelf_zigbee_apply_router_memory(void);
esp_err_t shelf_zigbee_apply_end_device_memory(void);

#ifdef __cplusplus
}
#endif
