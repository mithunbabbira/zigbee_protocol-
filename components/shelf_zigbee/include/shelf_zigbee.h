/*
 * Shared shelf Zigbee constants, stack tuning, and UART gateway protocol.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SHELF_ZIGBEE_STORAGE_PARTITION_NAME "zb_storage"

#define SHELF_PRIMARY_CHANNEL_MASK   ((1U << 15))
#define SHELF_SECONDARY_CHANNEL_MASK (0U)

#define SHELF_HA_LIGHT_EP_ID    (10)
#define SHELF_HA_GATEWAY_EP_ID  (1)

#define SHELF_DEFAULT_GROUP_ID  (0x0001U)

#define SHELF_MANUFACTURER_NAME "\x09""SHELF_MGMT"
#define SHELF_MODEL_IDENTIFIER  "\x07" CONFIG_IDF_TARGET

#define SHELF_COORDINATOR_SHORT_ADDR (0x0000U)

#define SHELF_NETWORK_SIZE       (256U)
#define SHELF_BUFFER_POOL_ZC     (200U)
#define SHELF_BUFFER_POOL_ZR     (160U)
#define SHELF_BIND_TABLE_SIZE    (32U)
#define SHELF_MAX_CHILDREN_ZC    (50U)
#define SHELF_MAX_CHILDREN_ZR    (20U)

#define SHELF_UART_LINE_MAX (128U)

typedef enum {
    SHELF_GATEWAY_CMD_NONE = 0,
    SHELF_GATEWAY_CMD_NODE_SEND,
    SHELF_GATEWAY_CMD_GROUP_SEND,
    SHELF_GATEWAY_CMD_NODE_SCAN,
} shelf_gateway_cmd_type_t;

typedef struct {
    shelf_gateway_cmd_type_t type;
    uint16_t addr;
    uint8_t state;
} shelf_gateway_cmd_t;

esp_err_t shelf_zigbee_apply_coordinator_memory(void);
esp_err_t shelf_zigbee_apply_router_memory(void);

bool shelf_gateway_parse_line(const char *line, shelf_gateway_cmd_t *cmd);
void shelf_gateway_format_node_recv(uint16_t short_addr, uint8_t state, char *out, size_t out_len);
void shelf_gateway_format_node_join(uint16_t short_addr, char *out, size_t out_len);
void shelf_gateway_format_node_leave(uint16_t short_addr, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
