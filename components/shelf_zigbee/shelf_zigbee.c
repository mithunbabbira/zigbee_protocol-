/*
 * Shared shelf Zigbee stack tuning and UART protocol helpers.
 */
#include <stdio.h>
#include <string.h>

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

bool shelf_gateway_parse_line(const char *line, shelf_gateway_cmd_t *cmd)
{
    if (line == NULL || cmd == NULL) {
        return false;
    }

    memset(cmd, 0, sizeof(*cmd));
    cmd->type = SHELF_GATEWAY_CMD_NONE;

    unsigned int addr = 0;
    unsigned int state = 0;

    if (sscanf(line, "NODE_SEND:%x:STATE:%u", &addr, &state) == 2 ||
        sscanf(line, "NODE_SEND:0x%x:STATE:%u", &addr, &state) == 2) {
        cmd->type  = SHELF_GATEWAY_CMD_NODE_SEND;
        cmd->addr  = (uint16_t)addr;
        cmd->state = (uint8_t)(state ? 1 : 0);
        return true;
    }

    if (sscanf(line, "GROUP_SEND:%x:STATE:%u", &addr, &state) == 2 ||
        sscanf(line, "GROUP_SEND:0x%x:STATE:%u", &addr, &state) == 2) {
        cmd->type  = SHELF_GATEWAY_CMD_GROUP_SEND;
        cmd->addr  = (uint16_t)addr;
        cmd->state = (uint8_t)(state ? 1 : 0);
        return true;
    }

    if (strcmp(line, "NODE_SCAN") == 0) {
        cmd->type = SHELF_GATEWAY_CMD_NODE_SCAN;
        return true;
    }

    return false;
}

void shelf_gateway_format_node_recv(uint16_t short_addr, uint8_t state, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }

    snprintf(out, out_len, "NODE_RECV:0x%04x:STATE:%u\n", short_addr, state ? 1U : 0U);
}

void shelf_gateway_format_node_join(uint16_t short_addr, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }

    snprintf(out, out_len, "NODE_JOIN:0x%04x\n", short_addr);
}

void shelf_gateway_format_node_leave(uint16_t short_addr, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }

    snprintf(out, out_len, "NODE_LEAVE:0x%04x\n", short_addr);
}
