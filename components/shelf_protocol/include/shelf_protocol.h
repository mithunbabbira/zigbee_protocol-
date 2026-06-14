/*
 * Put-to-Light protocol constants and JSON-line helpers (Pi <-> coordinator).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHELF_PROTOCOL_LINE_MAX        (256U)
#define SHELF_CLUSTER_EVENTS_ID        (0xFC01U)
#define SHELF_CMD_BUTTON_PRESSED_ID    (0x01U)
#define SHELF_CMD_HEARTBEAT_ID         (0x02U)
#define SHELF_ATTR_HEARTBEAT_SEQ_ID    (0x0000U)

#define SHELF_HA_LIGHT_EP_ID           (10U)
#define SHELF_HA_GATEWAY_EP_ID         (1U)
#define SHELF_COORDINATOR_SHORT_ADDR   (0x0000U)

#define SHELF_PRIMARY_CHANNEL_MASK     ((1U << 15))
#define SHELF_SECONDARY_CHANNEL_MASK (0U)

#define SHELF_MANUFACTURER_NAME        "\x09""SHELF_MGMT"
#define SHELF_MODEL_IDENTIFIER         "\x0a""SHELF-NODE"

typedef enum {
    SHELF_DOWNSTREAM_CMD_NONE = 0,
    SHELF_DOWNSTREAM_CMD_TURN_LED_ON,
    SHELF_DOWNSTREAM_CMD_TURN_LED_OFF,
    SHELF_DOWNSTREAM_CMD_BLINK_LED,
    SHELF_DOWNSTREAM_CMD_PERMIT_JOIN,
} shelf_downstream_cmd_t;

typedef struct {
    shelf_downstream_cmd_t cmd;
    uint8_t ieee[8];
    uint32_t duration_ms;
    uint16_t permit_join_sec;
} shelf_downstream_msg_t;

void shelf_protocol_ieee_to_string(const uint8_t ieee[8], char *out, size_t out_len);
bool shelf_protocol_parse_ieee_string(const char *text, uint8_t ieee[8]);

bool shelf_protocol_parse_downstream_line(const char *line, shelf_downstream_msg_t *msg);

void shelf_protocol_format_node_online(char *out, size_t out_len, const uint8_t ieee[8], uint16_t short_addr);
void shelf_protocol_format_node_offline(char *out, size_t out_len, const uint8_t ieee[8]);
void shelf_protocol_format_button_pressed(char *out, size_t out_len, const uint8_t ieee[8]);
void shelf_protocol_format_heartbeat(char *out, size_t out_len, const uint8_t ieee[8], uint32_t seq);
void shelf_protocol_format_led_state(char *out, size_t out_len, const uint8_t ieee[8], uint8_t state);
void shelf_protocol_format_cmd_failed(char *out, size_t out_len, const uint8_t ieee[8], const char *reason);

#ifdef __cplusplus
}
#endif
