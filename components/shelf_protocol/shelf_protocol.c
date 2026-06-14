/*
 * JSON-line protocol helpers for Put-to-Light gateway transport.
 */
#include <stdio.h>
#include <string.h>

#include "shelf_protocol.h"

static bool parse_hex_nibble(char c, uint8_t *out)
{
    if (c >= '0' && c <= '9') {
        *out = (uint8_t)(c - '0');
        return true;
    }
    if (c >= 'a' && c <= 'f') {
        *out = (uint8_t)(c - 'a' + 10);
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        *out = (uint8_t)(c - 'A' + 10);
        return true;
    }
    return false;
}

void shelf_protocol_ieee_to_string(const uint8_t ieee[8], char *out, size_t out_len)
{
    if (out == NULL || out_len < 19U || ieee == NULL) {
        return;
    }

    snprintf(out, out_len, "0x%02x%02x%02x%02x%02x%02x%02x%02x", ieee[7], ieee[6], ieee[5], ieee[4], ieee[3],
             ieee[2], ieee[1], ieee[0]);
}

bool shelf_protocol_parse_ieee_string(const char *text, uint8_t ieee[8])
{
    if (text == NULL || ieee == NULL) {
        return false;
    }

    const char *hex = text;
    if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex += 2;
    }

    if (strlen(hex) != 16U) {
        return false;
    }

    for (size_t i = 0; i < 8U; i++) {
        uint8_t hi = 0;
        uint8_t lo = 0;
        if (!parse_hex_nibble(hex[i * 2U], &hi) || !parse_hex_nibble(hex[i * 2U + 1U], &lo)) {
            return false;
        }
        ieee[7U - i] = (uint8_t)((hi << 4U) | lo);
    }

    return true;
}

static const char *find_json_string_value(const char *line, const char *key)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *pos = strstr(line, pattern);
    if (pos == NULL) {
        return NULL;
    }

    pos = strchr(pos + strlen(pattern), ':');
    if (pos == NULL) {
        return NULL;
    }

    pos = strchr(pos, '"');
    if (pos == NULL) {
        return NULL;
    }

    return pos + 1;
}

static bool copy_json_string(const char *start, char *out, size_t out_len)
{
    if (start == NULL || out == NULL || out_len == 0U) {
        return false;
    }

    size_t i = 0;
    while (start[i] != '\0' && start[i] != '"' && i + 1U < out_len) {
        out[i] = start[i];
        i++;
    }
    out[i] = '\0';
    return i > 0U;
}

static uint32_t parse_json_uint(const char *line, const char *key, uint32_t fallback)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *pos = strstr(line, pattern);
    if (pos == NULL) {
        return fallback;
    }

    pos = strchr(pos + strlen(pattern), ':');
    if (pos == NULL) {
        return fallback;
    }

    while (*pos != '\0' && (*pos < '0' || *pos > '9')) {
        pos++;
    }

    return (uint32_t)strtoul(pos, NULL, 10);
}

bool shelf_protocol_parse_downstream_line(const char *line, shelf_downstream_msg_t *msg)
{
    if (line == NULL || msg == NULL) {
        return false;
    }

    memset(msg, 0, sizeof(*msg));
    msg->cmd = SHELF_DOWNSTREAM_CMD_NONE;
    msg->duration_ms = 3000U;
    msg->permit_join_sec = 180U;

    char cmd_name[32] = {0};
    if (!copy_json_string(find_json_string_value(line, "cmd"), cmd_name, sizeof(cmd_name))) {
        return false;
    }

    if (strcmp(cmd_name, "TURN_LED_ON") == 0) {
        msg->cmd = SHELF_DOWNSTREAM_CMD_TURN_LED_ON;
    } else if (strcmp(cmd_name, "TURN_LED_OFF") == 0) {
        msg->cmd = SHELF_DOWNSTREAM_CMD_TURN_LED_OFF;
    } else if (strcmp(cmd_name, "BLINK_LED") == 0) {
        msg->cmd = SHELF_DOWNSTREAM_CMD_BLINK_LED;
        msg->duration_ms = parse_json_uint(line, "duration_ms", 3000U);
    } else if (strcmp(cmd_name, "PERMIT_JOIN") == 0) {
        msg->cmd = SHELF_DOWNSTREAM_CMD_PERMIT_JOIN;
        msg->permit_join_sec = (uint16_t)parse_json_uint(line, "duration", 180U);
        return true;
    } else {
        return false;
    }

    char ieee_text[24] = {0};
    if (!copy_json_string(find_json_string_value(line, "ieee"), ieee_text, sizeof(ieee_text))) {
        return false;
    }

    return shelf_protocol_parse_ieee_string(ieee_text, msg->ieee);
}

void shelf_protocol_format_node_online(char *out, size_t out_len, const uint8_t ieee[8], uint16_t short_addr)
{
    char ieee_text[24];
    shelf_protocol_ieee_to_string(ieee, ieee_text, sizeof(ieee_text));
    snprintf(out, out_len, "{\"event\":\"NODE_ONLINE\",\"ieee\":\"%s\",\"short\":\"0x%04x\"}\n", ieee_text, short_addr);
}

void shelf_protocol_format_node_offline(char *out, size_t out_len, const uint8_t ieee[8])
{
    char ieee_text[24];
    shelf_protocol_ieee_to_string(ieee, ieee_text, sizeof(ieee_text));
    snprintf(out, out_len, "{\"event\":\"NODE_OFFLINE\",\"ieee\":\"%s\"}\n", ieee_text);
}

void shelf_protocol_format_button_pressed(char *out, size_t out_len, const uint8_t ieee[8])
{
    char ieee_text[24];
    shelf_protocol_ieee_to_string(ieee, ieee_text, sizeof(ieee_text));
    snprintf(out, out_len, "{\"event\":\"BUTTON_PRESSED\",\"ieee\":\"%s\"}\n", ieee_text);
}

void shelf_protocol_format_heartbeat(char *out, size_t out_len, const uint8_t ieee[8], uint32_t seq)
{
    char ieee_text[24];
    shelf_protocol_ieee_to_string(ieee, ieee_text, sizeof(ieee_text));
    snprintf(out, out_len, "{\"event\":\"HEARTBEAT\",\"ieee\":\"%s\",\"seq\":%lu}\n", ieee_text, (unsigned long)seq);
}

void shelf_protocol_format_led_state(char *out, size_t out_len, const uint8_t ieee[8], uint8_t state)
{
    char ieee_text[24];
    shelf_protocol_ieee_to_string(ieee, ieee_text, sizeof(ieee_text));
    snprintf(out, out_len, "{\"event\":\"LED_STATE\",\"ieee\":\"%s\",\"state\":%u}\n", ieee_text, state ? 1U : 0U);
}

void shelf_protocol_format_cmd_failed(char *out, size_t out_len, const uint8_t ieee[8], const char *reason)
{
    char ieee_text[24];
    shelf_protocol_ieee_to_string(ieee, ieee_text, sizeof(ieee_text));
    snprintf(out, out_len, "{\"event\":\"CMD_FAILED\",\"ieee\":\"%s\",\"reason\":\"%s\"}\n", ieee_text,
             reason != NULL ? reason : "unknown");
}
