#include <string.h>

#include "device_registry.h"
#include "shelf_protocol.h"

static device_entry_t s_devices[DEVICE_REGISTRY_MAX];
static size_t s_device_count;
static device_registry_event_cb_t s_event_cb;
static void *s_event_ctx;

static int device_registry_index_by_ieee(const uint8_t ieee[8])
{
    for (size_t i = 0; i < s_device_count; i++) {
        if (memcmp(s_devices[i].ieee, ieee, 8) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int device_registry_index_by_short(uint16_t short_addr)
{
    for (size_t i = 0; i < s_device_count; i++) {
        if (s_devices[i].short_addr == short_addr) {
            return (int)i;
        }
    }
    return -1;
}

static void device_registry_emit(const char *json_line)
{
    if (s_event_cb != NULL && json_line != NULL) {
        s_event_cb(json_line, s_event_ctx);
    }
}

void device_registry_init(device_registry_event_cb_t cb, void *user_ctx)
{
    s_event_cb = cb;
    s_event_ctx = user_ctx;
    s_device_count = 0;
    memset(s_devices, 0, sizeof(s_devices));
}

void device_registry_upsert(const uint8_t ieee[8], uint16_t short_addr)
{
    if (ieee == NULL) {
        return;
    }

    char line[SHELF_PROTOCOL_LINE_MAX];
    int idx = device_registry_index_by_ieee(ieee);
    if (idx >= 0) {
        s_devices[idx].short_addr = short_addr;
        s_devices[idx].online = true;
        s_devices[idx].last_heartbeat_ms = 0;
        shelf_protocol_format_node_online(line, sizeof(line), ieee, short_addr);
        device_registry_emit(line);
        return;
    }

    if (s_device_count >= DEVICE_REGISTRY_MAX) {
        return;
    }

    device_entry_t *entry = &s_devices[s_device_count++];
    memcpy(entry->ieee, ieee, 8);
    entry->short_addr = short_addr;
    entry->online = true;
    entry->last_heartbeat_ms = 0;

    shelf_protocol_format_node_online(line, sizeof(line), ieee, short_addr);
    device_registry_emit(line);
}

void device_registry_remove_by_short(uint16_t short_addr)
{
    int idx = device_registry_index_by_short(short_addr);
    if (idx < 0) {
        return;
    }

    char line[SHELF_PROTOCOL_LINE_MAX];
    shelf_protocol_format_node_offline(line, sizeof(line), s_devices[idx].ieee);
    device_registry_emit(line);

    if ((size_t)idx + 1U < s_device_count) {
        s_devices[idx] = s_devices[s_device_count - 1U];
    }
    s_device_count--;
}

void device_registry_mark_heartbeat(const uint8_t ieee[8], uint32_t seq)
{
    int idx = device_registry_index_by_ieee(ieee);
    if (idx < 0) {
        return;
    }

    s_devices[idx].online = true;
    s_devices[idx].last_heartbeat_ms = seq;

    char line[SHELF_PROTOCOL_LINE_MAX];
    shelf_protocol_format_heartbeat(line, sizeof(line), ieee, seq);
    device_registry_emit(line);
}

bool device_registry_find_by_ieee(const uint8_t ieee[8], device_entry_t *out)
{
    int idx = device_registry_index_by_ieee(ieee);
    if (idx < 0) {
        return false;
    }
    if (out != NULL) {
        *out = s_devices[idx];
    }
    return true;
}

bool device_registry_find_by_short(uint16_t short_addr, device_entry_t *out)
{
    int idx = device_registry_index_by_short(short_addr);
    if (idx < 0) {
        return false;
    }
    if (out != NULL) {
        *out = s_devices[idx];
    }
    return true;
}

bool device_registry_resolve_ieee_to_short(const uint8_t ieee[8], uint16_t *short_addr)
{
    device_entry_t entry;
    if (!device_registry_find_by_ieee(ieee, &entry)) {
        return false;
    }
    if (short_addr != NULL) {
        *short_addr = entry.short_addr;
    }
    return true;
}

void device_registry_check_timeouts(uint32_t now_ms, uint32_t timeout_ms)
{
    (void)now_ms;
    (void)timeout_ms;
}
