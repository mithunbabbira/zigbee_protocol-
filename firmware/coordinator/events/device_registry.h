#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_REGISTRY_MAX (32U)

typedef struct {
    uint8_t ieee[8];
    uint16_t short_addr;
    uint32_t last_heartbeat_ms;
    bool online;
} device_entry_t;

typedef void (*device_registry_event_cb_t)(const char *json_line, void *user_ctx);

void device_registry_init(device_registry_event_cb_t cb, void *user_ctx);
void device_registry_upsert(const uint8_t ieee[8], uint16_t short_addr);
void device_registry_remove_by_short(uint16_t short_addr);
void device_registry_mark_heartbeat(const uint8_t ieee[8], uint32_t seq);
bool device_registry_find_by_ieee(const uint8_t ieee[8], device_entry_t *out);
bool device_registry_find_by_short(uint16_t short_addr, device_entry_t *out);
bool device_registry_resolve_ieee_to_short(const uint8_t ieee[8], uint16_t *short_addr);
void device_registry_check_timeouts(uint32_t now_ms, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
