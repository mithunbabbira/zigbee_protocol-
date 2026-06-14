/*
 * Shared shelf Zigbee stack tuning.
 */
#include "esp_zigbee.h"
#include "ezbee/core.h"

#include "shelf_zigbee.h"

esp_err_t shelf_zigbee_apply_coordinator_memory(void)
{
    ezb_mem_config_t mem_cfg = {
        .buffer_pool_size           = SHELF_BUFFER_POOL_ZC,
        .address_table_size         = SHELF_NETWORK_SIZE,
        .neighbor_table_size        = SHELF_NETWORK_SIZE,
        .route_table_size           = 64,
        .route_discovery_table_size = 16,
        .route_record_table_size    = 16,
        .aps_key_pair_set_size      = 32,
        .aps_bind_table_src_size    = SHELF_BIND_TABLE_SIZE,
        .aps_bind_table_dst_size    = SHELF_BIND_TABLE_SIZE,
    };

    return (ezb_config_memory(&mem_cfg) == EZB_ERR_NONE) ? ESP_OK : ESP_FAIL;
}

esp_err_t shelf_zigbee_apply_router_memory(void)
{
    ezb_mem_config_t mem_cfg = {
        .buffer_pool_size           = SHELF_BUFFER_POOL_ZR,
        .address_table_size         = 128,
        .neighbor_table_size        = 128,
        .route_table_size           = 32,
        .route_discovery_table_size = 8,
        .route_record_table_size    = 8,
        .aps_key_pair_set_size      = 16,
        .aps_bind_table_src_size    = 8,
        .aps_bind_table_dst_size    = 8,
    };

    return (ezb_config_memory(&mem_cfg) == EZB_ERR_NONE) ? ESP_OK : ESP_FAIL;
}

esp_err_t shelf_zigbee_apply_end_device_memory(void)
{
    ezb_mem_config_t mem_cfg = {
        .buffer_pool_size           = SHELF_BUFFER_POOL_ZED,
        .address_table_size         = 32,
        .neighbor_table_size        = 16,
        .route_table_size           = 0,
        .route_discovery_table_size = 0,
        .route_record_table_size    = 0,
        .aps_key_pair_set_size      = 8,
        .aps_bind_table_src_size    = 4,
        .aps_bind_table_dst_size    = 4,
    };

    return (ezb_config_memory(&mem_cfg) == EZB_ERR_NONE) ? ESP_OK : ESP_FAIL;
}
