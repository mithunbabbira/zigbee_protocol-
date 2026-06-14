#pragma once

#include "esp_zigbee.h"
#include "shelf_zigbee.h"

#define SHELF_ZIGBEE_ZR_CONFIG()                        \
    {                                                   \
        .device_type = EZB_NWK_DEVICE_TYPE_ROUTER,      \
        .install_code_policy = false,                   \
        .zczr_config = {                                \
            .max_children = SHELF_MAX_CHILDREN_ZR,      \
        },                                              \
    }

#define SHELF_ZIGBEE_ZED_CONFIG()                       \
    {                                                   \
        .device_type = EZB_NWK_DEVICE_TYPE_END_DEVICE,  \
        .install_code_policy = false,                   \
        .zed_config = {                                 \
            .ed_timeout = EZB_NWK_ED_TIMEOUT_64MIN,     \
            .keep_alive = 3000,                          \
        },                                              \
    }

#if CONFIG_SHELF_DEVICE_ROLE_ROUTER
#define SHELF_ZIGBEE_DEVICE_CONFIG() SHELF_ZIGBEE_ZR_CONFIG()
#else
#define SHELF_ZIGBEE_DEVICE_CONFIG() SHELF_ZIGBEE_ZED_CONFIG()
#endif

#if CONFIG_SOC_IEEE802154_SUPPORTED
#define SHELF_ZIGBEE_PLATFORM_CONFIG()                                     \
    {                                                                        \
        .storage_partition_name = SHELF_ZIGBEE_STORAGE_PARTITION_NAME,       \
        .radio_config = {                                                    \
            .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE,                      \
        },                                                                   \
    }
#else
#warning "IEEE 802.15.4 is required for shelf nodes"
#endif

#define SHELF_ZIGBEE_DEFAULT_CONFIG()                    \
    {                                                    \
        .device_config = SHELF_ZIGBEE_DEVICE_CONFIG(),   \
        .platform_config = SHELF_ZIGBEE_PLATFORM_CONFIG(), \
    }
