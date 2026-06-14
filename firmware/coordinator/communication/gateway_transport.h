#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*gateway_transport_line_cb_t)(const char *line, void *user_ctx);

void gateway_transport_init(gateway_transport_line_cb_t cb, void *user_ctx);
void gateway_transport_start(void);
esp_err_t gateway_transport_console_init(void);
void gateway_transport_emit(const char *json_line);

#ifdef __cplusplus
}
#endif
