#include "esp_log.h"

#include "device_registry.h"
#include "gateway_transport.h"
#include "message_handler.h"
#include "shelf_protocol.h"
#include "zigbee_coordinator.h"

static const char *TAG = "MSG_HANDLER";

void message_handler_init(void)
{
    device_registry_init(gateway_transport_emit, NULL);
}

void message_handler_emit_button_pressed(const uint8_t ieee[8])
{
    char line[SHELF_PROTOCOL_LINE_MAX];
    shelf_protocol_format_button_pressed(line, sizeof(line), ieee);
    gateway_transport_emit(line);
}

void message_handler_emit_led_state(const uint8_t ieee[8], uint8_t state)
{
    char line[SHELF_PROTOCOL_LINE_MAX];
    shelf_protocol_format_led_state(line, sizeof(line), ieee, state);
    gateway_transport_emit(line);
}

void message_handler_on_gateway_line(const char *line)
{
    shelf_downstream_msg_t msg;
    if (!shelf_protocol_parse_downstream_line(line, &msg)) {
        ESP_LOGW(TAG, "Invalid gateway line: %s", line);
        return;
    }

    switch (msg.cmd) {
    case SHELF_DOWNSTREAM_CMD_TURN_LED_ON:
        zigbee_coordinator_send_on_off(msg.ieee, true);
        break;
    case SHELF_DOWNSTREAM_CMD_TURN_LED_OFF:
        zigbee_coordinator_send_on_off(msg.ieee, false);
        break;
    case SHELF_DOWNSTREAM_CMD_BLINK_LED:
        zigbee_coordinator_send_blink(msg.ieee, msg.duration_ms);
        break;
    case SHELF_DOWNSTREAM_CMD_PERMIT_JOIN:
        zigbee_coordinator_open_network(msg.permit_join_sec);
        break;
    default:
        ESP_LOGW(TAG, "Unhandled command");
        break;
    }
}
