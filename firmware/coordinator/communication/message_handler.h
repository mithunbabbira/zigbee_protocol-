#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void message_handler_init(void);
void message_handler_on_gateway_line(const char *line);
void message_handler_emit_button_pressed(const uint8_t ieee[8]);
void message_handler_emit_led_state(const uint8_t ieee[8], uint8_t state);

#ifdef __cplusplus
}
#endif
