# Put-to-Light Architecture

**Target platform:** Raspberry Pi 5 + ESP32-C6 Coordinator (USB) + ESP32-C6 Shelf Nodes  
**Stack:** Espressif ESP-IDF + [esp-zigbee-sdk](https://github.com/espressif/esp-zigbee-sdk) (ZBOSS)  
**Principle:** Nodes are dumb I/O devices. The Pi/backend owns all shelf identity.

---

## Current System Issues

### What is broken

| Symptom | Root cause |
|---------|------------|
| Web UI ON/OFF buttons disabled / no effect | `mqtt_bridge.py` `_derive_online_flag()` treated `interview_completed=False` as offline. Z2M labels ESP shelves **"Not supported (EndDevice)"** when interview fails, so all buttons were disabled even though MQTT state/control still worked. |
| Button press not visible in web app | Same online-flag bug blocks UI updates; button press relies on On/Off attribute reporting via Z2M (no custom cluster yet). If Z2M or web app is stopped, nothing propagates. |
| Mixed coordinator backends | Code supports **two incompatible stacks**: (A) ESP32 coordinator + UART text protocol using **short addresses**, (B) SONOFF dongle + Zigbee2MQTT + MQTT using **IEEE addresses**. README points to (B); user hardware target is (A) Pi + ESP32-C6. |
| Address identity confusion | Serial protocol emits `NODE_RECV:0x1234` (16-bit short). Backend/web must use IEEE `0x1051dbfffe1c4a78`. Short addresses change on rejoin — unsuitable for warehouse mapping. |
| Over-engineered coordinator | 680-line monolith: neighbor rescans, liveness pings, group auto-provision, bind tables, duplicate `NODE_JOIN` broadcasts, UART parsing — most unnecessary for a fixed warehouse network. |
| Zigbee2MQTT fragility | Requires `external_converters/shelf_node.mjs`, endpoint 10 mapping, `disableDefaultResponse`, group membership automation, Ember adapter tuning — third-party stack on top of third-party dongle. |
| `permit_join: false` | `gateway/zigbee2mqtt/configuration.yaml` blocks new joins unless manually enabled in Z2M frontend. |
| Button vs LED conflation | Button toggles local LED **and** relies on On/Off attribute reporting to signal "button pressed" — backend cannot distinguish operator acknowledgment from remote LED command. |

### What to remove

- **Zigbee2MQTT path** for production (dongle, external converters, `mqtt_bridge.py` group/configure hacks)
- **Legacy UART text protocol** (`NODE_SEND:0xADDR`, short-address `NODE_RECV`)
- **Coordinator liveness scan / neighbor rescan loops** (replace with heartbeat + Device Annce/Leave signals)
- **Automatic group membership** on nodes and coordinator (`SHELF_DEFAULT_GROUP_ID`, groupcast path)
- **Dual-backend web app** (`--backend mqtt|serial`) — single Pi ↔ coordinator transport
- **`CONFIG_SHELF_JOIN_ALL_CHANNELS`** for production (use fixed channel 15 matching coordinator)
- **Kconfig role sprawl** without a single deployment config file

### What to keep

- **HA On/Off light data model** on shelf nodes (Espressif official pattern for LED on/off)
- **`status_led` and `switch_driver` components** (proven GPIO/WS2812 handling)
- **`shelf_zigbee` memory tuning** macros (adapt values from YAML)
- **BDB commissioning flow** (formation on coordinator, steering on nodes)
- **ZDO bind on join** (Espressif HA switch↔light pattern) — simplified, IEEE-centric
- **Web UI shell** (`gateway/web_app/static/index.html`) — rewire API to Pi backend over IEEE

---

## 1. Network Architecture

### Topology

```
┌─────────────────────────────────────────────────────────────────┐
│  Raspberry Pi 5                                                 │
│  ┌──────────────┐    USB      ┌─────────────────────────────┐ │
│  │ Backend /    │◄───────────►│ ESP32-C6 Coordinator (ZC)   │ │
│  │ Web App      │  JSON lines │ Channel 15, PAN 0x1A2B        │ │
│  └──────────────┘             └──────────────┬──────────────┘ │
└──────────────────────────────────────────────│──────────────────┘
                                               │ 802.15.4
                    ┌──────────────────────────┼──────────────────────────┐
                    │                          │                          │
             ┌──────▼──────┐           ┌──────▼──────┐           ┌──────▼──────┐
             │ Shelf Node  │           │ Shelf Node  │           │ Shelf Node  │
             │ ZED (×N)    │           │ ZED         │           │ ZR (×0–1)   │
             │ LED+Button  │           │ LED+Button  │           │ range only  │
             └─────────────┘           └─────────────┘           └─────────────┘
```

### Role assignment

| Device | Zigbee role | SDK config | Rationale |
|--------|-------------|------------|-----------|
| ESP32-C6 on Pi | **Coordinator (ZC)** | `ESP_ZIGBEE_ZC_CONFIG()` / `EZB_NWK_DEVICE_TYPE_COORDINATOR` | Forms network, permits join, routes to children |
| Shelf nodes (default) | **End Device (ZED)** | `ESP_ZIGBEE_ZED_CONFIG()`, `keep_alive` 3000 ms | Lower RAM, sufficient for powered shelf with parent polling; official Espressif pattern for leaf devices |
| One optional shelf | **Router (ZR)** | `ESP_ZIGBEE_ZR_CONFIG()`, `max_children` 20 | Only when coordinator is far from a shelf cluster; **at most one** per aisle |

**Do not** make every shelf a router — increases chatter and coordinator child table pressure.

### Coordinator responsibilities

1. Form/maintain Zigbee network (channel, PAN ID, extended PAN ID from `coordinator_config.yaml`)
2. Open/close permit join on backend command
3. Handle `EZB_ZDO_SIGNAL_DEVICE_ANNCE` → emit `NODE_ONLINE` with **IEEE address**
4. Handle `EZB_ZDO_SIGNAL_LEAVE_INDICATION` → emit offline event
5. Receive upstream: `BUTTON_PRESSED`, `HEARTBEAT` (custom cluster) and On/Off reports
6. Send downstream: `TURN_LED_ON`, `TURN_LED_OFF`, `BLINK_LED` via genOnOff + Identify
7. Expose clean **JSON-lines gateway** to Pi (no short-address commands)

### Shelf node responsibilities

1. Join network via `EZB_BDB_MODE_NETWORK_STEERING` on fixed channel
2. Register HA On/Off server (endpoint 10) + custom events cluster (0xFC01)
3. Drive LED from downstream On/Off commands
4. On button press: toggle LED locally **and** send `BUTTON_PRESSED` (custom command) — do not rely on On/Off report alone
5. Send periodic `HEARTBEAT` (custom cluster report)
6. Store **zero** shelf names or warehouse IDs

### Message flow

**Downstream (pick task → light shelf):**

```
Pi backend → JSON {"cmd":"TURN_LED_ON","ieee":"0x1051..."}
  → Coordinator → ZCL On Command (genOnOff, ep 10)
  → Shelf Node → LedController.on()
```

**Upstream (operator confirms pick):**

```
Button press → ButtonController → ZigbeeShelfNode.send_button_pressed()
  → Custom cluster cmd 0xFC01 / cmd_id=0x01
  → Coordinator MessageHandler → JSON {"event":"BUTTON_PRESSED","ieee":"0x1051..."}
  → Pi backend → map IEEE → shelf name → WMS
```

**Heartbeat / online:**

```
Shelf timer → HEARTBEAT report (0xFC01 attr/report)
Coordinator miss > heartbeat_timeout → mark offline in DeviceRegistry
Device Annce on join/rejoin → NODE_ONLINE event with IEEE
```

---

## 2. Coordinator Configuration

**File:** `firmware/coordinator/config/coordinator_config.yaml`

| Parameter | Value | Explanation |
|-----------|-------|-------------|
| `role` | `coordinator` | Device forms the Zigbee network; only one per deployment |
| `pan_id` | `0x1A2B` | 16-bit network ID; fixed for warehouse; nodes rejoin this PAN |
| `channel` | `15` | 2.4 GHz channel 15 (0-based API uses bit mask `(1<<15)`); single channel = ~2 s faster rejoin per Espressif guidance |
| `extended_pan_id` | `0x1122334455667788` | 64-bit unique network key identity; prevents accidental cross-join with neighboring warehouses |
| `permit_join` | `true` | Backend can commission new/replacement nodes |
| `permit_join_duration_sec` | `180` | Join window auto-closes to prevent drive-by joins |
| `max_children` | `32` | Direct children limit on coordinator; size for ~30 shelves + 1 router |
| `network_open_duration_sec` | `180` | Firmware `ezb_bdb_open_network()` duration on boot |
| `heartbeat_timeout_sec` | `90` | 3× node heartbeat interval; marks stale nodes offline |
| `install_code_policy` | `false` | Open joining for warehouse swap-out workflow |
| `gateway.protocol` | `json_lines` | Machine-readable Pi transport |

**Espressif SDK alignment:**

- Initialize: `esp_zigbee_init()` → `ezb_bdb_set_primary_channel_set(1<<15)` → register HA on/off **switch** endpoint (client) → `esp_zigbee_start(false)` → `esp_zigbee_launch_mainloop()`
- Commission: `EZB_BDB_MODE_NETWORK_FORMATION` on first boot; `ezb_bdb_open_network(180)` on reboot
- All ZCL/ZDO calls wrapped in `esp_zigbee_lock_acquire()` / `release()` per SDK docs

---

## 3. Shelf Node Configuration

**File:** `firmware/shelf_node/config/shelf_node_config.yaml`

| Parameter | Value | Explanation |
|-----------|-------|-------------|
| `role` | `end_device` | Leaf node; joins via coordinator or optional router |
| `led_gpio` | `8` | WS2812 data line on ESP32-C6-DevKitM-1 |
| `button_gpio` | `9` | Active-low BOOT button |
| `heartbeat_interval_sec` | `30` | Periodic upstream keepalive for coordinator registry |
| `retry_count` | `3` | Retries for failed ZCL reports/commands before logging error |
| `keep_alive_ms` | `3000` | ZED parent poll interval (`esp_zigbee_zed_config_s.keep_alive`) |
| `join_channel_mask` | `15` | Scan only coordinator channel in production |
| `rejoin_on_boot` | `true` | `EZB_BDB_MODE_NETWORK_STEERING` after NVS restore |
| `endpoint_id` | `10` | HA light endpoint (matches existing builds) |
| `clusters.on_off` | `0x0006` | Standard LED control |
| `clusters.shelf_events` | `0xFC01` | Application events (above `EZB_ZCL_CLUSTER_ID_MIN_CUSTOM`) |

**No business mappings** — YAML contains only hardware and protocol constants.

---

## 4. Device Identification Strategy

### IEEE vs short address

| Address | Scope | Use |
|---------|-------|-----|
| **IEEE EUI-64** (`0x1051dbfffe1c4a78`) | Global, burned into chip | **Primary key** in Pi database, web UI, WMS integration |
| **Short address** (`0x3B4C`) | Network session, changes on rejoin | Internal to coordinator firmware only; never exposed to backend |

### Backend usage

```sql
-- Pi database (example)
CREATE TABLE shelves (
  ieee_address TEXT PRIMARY KEY,   -- 0x1051dbfffe1c4a78
  display_name TEXT NOT NULL,      -- "A-12-03"
  rack TEXT,
  last_seen TIMESTAMPTZ,
  led_state BOOLEAN
);
```

All gateway JSON uses `ieee` field. Backend resolves `display_name` at read time.

### Discovery

1. Coordinator emits on Device Annce:

```json
{"event":"NODE_ONLINE","ieee":"0x1051dbfffe1c4a78","short":"0x3b4c","ts":1718364000}
```

2. Pi backend inserts row with `ieee_address`; operator assigns `display_name` in admin UI
3. Unknown IEEE in `BUTTON_PRESSED` → queue for provisioning UI

### Replacement handling

1. Unplug failed shelf, plug replacement
2. Pi sends `{"cmd":"PERMIT_JOIN","duration":180}`
3. New node joins → new IEEE (or same module if reflashed without erase)
4. Operator updates DB mapping: old IEEE → new IEEE for that `display_name`
5. **No firmware reflash** required for name changes; **erase flash** only when moving between warehouses/PANs

---

## 5. Message Definitions

### Transport envelope (Pi ↔ Coordinator)

All messages are one JSON object per line terminated by `\n`.

### Coordinator → Node

| Message | ZCL implementation | Payload example |
|---------|-------------------|-----------------|
| `TURN_LED_ON` | `ezb_zcl_on_off_on_cmd_req()` → genOnOff | `{"cmd":"TURN_LED_ON","ieee":"0x1051dbfffe1c4a78"}` |
| `TURN_LED_OFF` | `ezb_zcl_on_off_off_cmd_req()` | `{"cmd":"TURN_LED_OFF","ieee":"0x1051dbfffe1c4a78"}` |
| `BLINK_LED` | Identify cluster `ezb_zcl_identify_trigger_effect_cmd_req()` effect 0x01 (blink) | `{"cmd":"BLINK_LED","ieee":"0x1051dbfffe1c4a78","duration_ms":3000}` |
| `PERMIT_JOIN` | `ezb_bdb_open_network(duration)` | `{"cmd":"PERMIT_JOIN","duration":180}` |

Coordinator resolves `ieee` → short address via `ezb_address_short_by_extended()`.

### Node → Coordinator

| Message | ZCL implementation | Payload example |
|---------|-------------------|-----------------|
| `BUTTON_PRESSED` | Custom cluster `0xFC01`, command ID `0x01`, no payload | `{"event":"BUTTON_PRESSED","ieee":"0x1051dbfffe1c4a78","ts":1718364012}` |
| `NODE_ONLINE` | Generated on `EZB_ZDO_SIGNAL_DEVICE_ANNCE` (coordinator-side) | `{"event":"NODE_ONLINE","ieee":"0x1051dbfffe1c4a78","short":"0x3b4c"}` |
| `HEARTBEAT` | Custom cluster `0xFC01`, attribute `0x0000` (uint32 seq) reporting | `{"event":"HEARTBEAT","ieee":"0x1051dbfffe1c4a78","seq":42}` |
| `LED_STATE` | genOnOff attribute report (optional, for UI sync) | `{"event":"LED_STATE","ieee":"0x1051dbfffe1c4a78","state":1}` |

### Custom cluster 0xFC01 layout

```
Cluster: 0xFC01 (server on shelf, client on coordinator)
Commands (server receives):
  0x01 BUTTON_PRESSED  — node→coord, instant on GPIO IRQ
Attributes (reportable):
  0x0000 heartbeat_seq — uint32, incremented each heartbeat interval
```

### BLINK behavior

Use standard **Identify** cluster (0x0003) on endpoint 10:

- Effect: `EZB_ZCL_IDENTIFY_EFFECT_BLINK` for `duration_ms / 1000` identify time
- Falls back to On/Off toggle loop if Identify not bound — prefer Identify for standards compliance

---

## 6. Clean Software Architecture

### Coordinator project structure

```
firmware/coordinator/
├── config/
│   └── coordinator_config.yaml
├── main/
│   ├── coordinator.c          # app_main, task creation
│   └── coordinator.h
└── components/
    ├── zigbee_coordinator/      # Stack init, signals, commissioning
    │   ├── zigbee_coordinator.c
    │   └── include/zigbee_coordinator.h
    ├── device_registry/         # IEEE↔short, online state, heartbeat tracking
    │   ├── device_registry.c
    │   └── include/device_registry.h
    ├── message_handler/         # JSON↔ZCL translation
    │   ├── message_handler.c
    │   └── include/message_handler.h
    └── gateway_transport/       # USB serial read/write JSON lines
        ├── gateway_transport.c
        └── include/gateway_transport.h

components/                      # Shared repo-level
├── shelf_protocol/              # Cluster IDs, command IDs, JSON schema helpers
├── board_config/                # Parse YAML → compile-time or boot-time constants
├── alarm_timer/                 # (existing)
└── shelf_zigbee/                # Memory macros (simplified)
```

### Shelf node project structure

```
firmware/shelf_node/
├── config/
│   └── shelf_node_config.yaml
├── main/
│   ├── shelf_node.c             # app_main only
│   └── shelf_node.h
└── components/
    ├── zigbee_shelf_node/       # Stack, endpoints, commissioning, ZCL handlers
    │   ├── zigbee_shelf_node.c
    │   └── include/zigbee_shelf_node.h
    ├── led_controller/          # Wraps status_led; on/off/blink
    │   ├── led_controller.c
    │   └── include/led_controller.h
    └── button_controller/       # Wraps switch_driver; debounce; callback
        ├── button_controller.c
        └── include/button_controller.h
```

### Module interaction

```
coordinator.c
  └─ zigbee_coordinator_init()
       ├─ device_registry_init()
       ├─ message_handler_init(registry)
       └─ gateway_transport_start(message_handler_dispatch)
  └─ zigbee_coordinator_start_task()

shelf_node.c
  └─ zigbee_shelf_node_init()
       ├─ led_controller_init()
       └─ button_controller_init(on_press → zigbee_shelf_node_send_button_pressed)
  └─ zigbee_shelf_node_start_task()
```

---

## 7. Class / Module Design

### Coordinator

#### `ZigbeeCoordinator`

| | |
|--|--|
| **Responsibility** | ESP Zigbee stack lifecycle, BDB commissioning, signal dispatch |
| **Public methods** | `init(config)`, `start()`, `open_network(duration)`, `send_on_off(ieee, bool)`, `send_blink(ieee, ms)`, `register_app_handler(callback)` |
| **Member variables** | `coordinator_config_t m_config`, `bool m_network_up`, `TaskHandle_t m_zigbee_task` |

#### `DeviceRegistry`

| | |
|--|--|
| **Responsibility** | Track joined nodes by IEEE; short-address cache; heartbeat timestamps |
| **Public methods** | `upsert(ieee, short_addr)`, `remove(ieee)`, `find_by_ieee(ieee)`, `find_by_short(short)`, `mark_heartbeat(ieee)`, `get_offline_devices(timeout)` |
| **Member variables** | `device_entry_t m_devices[MAX_DEVICES]`, `size_t m_count` |

```c
typedef struct {
    uint8_t ieee[8];
    uint16_t short_addr;
    uint32_t last_heartbeat_ms;
    bool online;
} device_entry_t;
```

#### `MessageHandler`

| | |
|--|--|
| **Responsibility** | Parse gateway JSON; emit gateway JSON; invoke ZCL via coordinator |
| **Public methods** | `handle_gateway_line(json)`, `emit_button_pressed(ieee)`, `emit_heartbeat(ieee, seq)`, `emit_node_online(ieee, short)` |
| **Member variables** | `DeviceRegistry *m_registry`, `ZigbeeCoordinator *m_coord`, `char m_line_buf[256]` |

### Shelf Node

#### `ZigbeeShelfNode`

| | |
|--|--|
| **Responsibility** | End device stack, HA On/Off + custom cluster endpoints, upstream send |
| **Public methods** | `init(config)`, `start()`, `send_button_pressed()`, `send_heartbeat()`, `on_downstream_on_off(bool)` |
| **Member variables** | `shelf_node_config_t m_config`, `uint8_t m_led_state`, `uint32_t m_heartbeat_seq` |

#### `LedController`

| | |
|--|--|
| **Responsibility** | GPIO/WS2812 actuation only |
| **Public methods** | `init(gpio)`, `set(bool on)`, `blink(uint32_t ms)` |
| **Member variables** | `gpio_num_t m_gpio`, `bool m_state`, `TimerHandle_t m_blink_timer` |

#### `ButtonController`

| | |
|--|--|
| **Responsibility** | Debounced button ISR → callback |
| **Public methods** | `init(gpio, callback)`, `deinit()` |
| **Member variables** | `gpio_num_t m_gpio`, `button_cb_t m_cb`, `switch_driver_handle_t m_handle` |

---

## 8. Naming Conventions

Follow [ESP-IDF style guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32contribute/style-guide.html):

| Element | Convention | Example |
|---------|------------|---------|
| Functions | `snake_case`, module prefix | `device_registry_find_by_ieee()` |
| Types | `snake_case` + `_t` suffix | `device_entry_t`, `shelf_node_config_t` |
| Enums | `UPPER_SNAKE` values, `_t` type | `SHELF_CMD_TURN_LED_ON` |
| Macros / constants | `UPPER_SNAKE` | `SHELF_CLUSTER_EVENTS_ID` |
| Files | `snake_case.c/.h` | `zigbee_coordinator.c` |
| Components | `snake_case` directory | `device_registry/` |
| Log tags | `UPPER_SNAKE` string | `static const char *TAG = "ZB_COORD";` |
| Config structs | `snake_case` + `_config_t` | `coordinator_config_t` |
| No Hungarian notation | — | `device_count` not `u8DeviceCount` |

---

## 9. Error Handling

### Network failures

| Condition | Detection | Response |
|-----------|-----------|----------|
| Formation failed | `EZB_BDB_SIGNAL_FORMATION` status ≠ SUCCESS | Retry after 1 s via `alarm_timer`; log warning |
| Steering failed | `EZB_BDB_SIGNAL_STEERING` status ≠ SUCCESS | Retry steering; shelf LED slow-blink fault pattern |
| ZCL command timeout | No Default Response / APS ack fail | Retry up to `retry_count`; emit `{"event":"CMD_FAILED","ieee":...}` to Pi |
| Coordinator USB disconnect | Pi serial EOF | Pi marks coordinator offline; queue commands |

### Offline detection

1. **Primary:** Heartbeat miss > `heartbeat_timeout_sec` → `DeviceRegistry` marks offline → Pi SSE update
2. **Secondary:** `EZB_ZDO_SIGNAL_LEAVE_INDICATION` → immediate offline
3. **Do not** use neighbor-table rescans or attribute-read pings (current code) — expensive and racy

### Retries

- **ZED → coordinator reports:** 3 retries, exponential backoff 100/200/400 ms
- **Coordinator → ZED commands:** 2 retries; if node is ZED, ensure `keep_alive` parent polling is active
- **Rejoin:** On `EZB_BDB_SIGNAL_STEERING` failure 5×, `ezb_factory_reset()` + NVS erase only via physical long-press (not automatic)

### Rejoin

- Nodes store network params in `zb_storage` NVS partition
- On boot: steering to fixed channel (not full mask scan)
- Coordinator on reboot: `ezb_bdb_open_network(180)` — nodes rejoin automatically
- New IEEE after module swap handled in backend mapping only

---

## 10. Final Recommendation

### Exact architecture for production warehouse

```
Pi 5 (FastAPI + PostgreSQL/SQLite)
  ↕ USB JSON-lines
ESP32-C6 Coordinator (Espressif ZC, ch 15, PAN 0x1A2B)
  ↕ 802.15.4
ESP32-C6 Shelf Nodes × N (ZED) + optional 1× ZR per aisle
```

### Config values (production)

| Setting | Coordinator | Shelf node |
|---------|-------------|------------|
| Role | ZC | ZED (1× ZR optional) |
| Channel | 15 | join mask channel 15 only |
| PAN ID | 0x1A2B | (learned on join) |
| Permit join | 180 s on demand | — |
| Heartbeat | timeout 90 s | interval 30 s |
| Keep-alive | — | 3000 ms |
| Endpoint | 1 (switch client) | 10 (light server) |

### Coding approach

1. **Follow Espressif HA on/off light + switch examples** verbatim for stack init, signal handler, and bind flow
2. **Add one custom cluster** (`0xFC01`) for `BUTTON_PRESSED` and `HEARTBEAT` — do not overload On/Off reports
3. **Replace UART text protocol** with JSON-lines using IEEE addresses
4. **Delete Zigbee2MQTT** from production path; keep in `gateway/` only as archived dev reference if needed
5. **Split monolithic `coordinator.c`** into 4 components (~150 lines each)
6. **Load YAML config** at coordinator boot from Pi or embed via `idf.py` cmake embed
7. **Pi backend** owns shelf names; firmware never stores them
8. **Test matrix:** button → Pi event < 200 ms; LED command → physical LED < 150 ms; rejoin after power cycle < 10 s on fixed channel

### Migration steps (from current repo)

1. Fix `mqtt_bridge` online detection for Z2M "Not supported" devices (done)
2. Flash coordinator firmware to ESP32-C6 on Pi; erase shelf NVS (`idf.py erase-flash`)
3. Run Pi gateway on USB serial JSON backend
4. Commission shelves with permit join; map IEEE addresses in admin UI
5. Remove `SHELF_DEFAULT_GROUP_ID` auto-join from `shelf_node.c`
6. Enable `send_button_pressed()` on custom cluster instead of toggle-only upstream

---

## References

- [ESP Zigbee SDK — Developing (ESP32-C6)](https://docs.espressif.com/projects/esp-zigbee-sdk/en/latest/esp32c6/developing.html)
- [ESP Zigbee SDK — Custom Cluster](https://docs.espressif.com/projects/esp-zigbee-sdk/en/latest/esp32c6/user-guide/zcl_custom.html)
- [ESP Zigbee SDK — FAQs (keep-alive, rejoin)](https://docs.espressif.com/projects/esp-zigbee-sdk/en/latest/esp32c6/faq.html)
- [ESP-IDF RF Coexistence (C6 Wi-Fi + 802.15.4)](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-guides/coexist.html)
