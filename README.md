# Put-to-Light (ESP32-C6)

Production stack: **Raspberry Pi / Mac backend + ESP32-C6 Zigbee Coordinator + ESP32-C6 Shelf Nodes**.

```
Web UI  →  USB JSON-lines  →  ESP32-C6 Coordinator  ⇄ 802.15.4 ⇄  ESP32-C6 Shelf Nodes
```

| Board | Firmware | USB to host |
|-------|----------|-------------|
| Coordinator dongle | `firmware/coordinator` | Yes |
| Shelf nodes | `firmware/shelf_node` (ZED) | Flash only |

Zigbee channel **15**, PAN **0x1A2B**. Shelf identity (names/racks) lives in the backend only — nodes expose IEEE addresses.

## Quick start

### 1. Build and flash coordinator

```bash
source ~/esp/esp-idf/export.sh
./scripts/flash_coordinator.sh /dev/cu.usbmodem1101
```

### 2. Flash shelf nodes (erase old network first)

```bash
./scripts/flash_shelf.sh end_device /dev/cu.usbmodem1101
```

Power shelf nodes from USB or external supply after flashing. Only the **coordinator** stays connected to the host for the gateway link.

### 3. Start web UI (serial JSON backend)

```bash
./scripts/run_web_master.sh /dev/cu.usbmodem1101
```

Open http://localhost:8080 — joined shelves appear with IEEE addresses. Map display names in your backend database.

### 4. Commission new shelves

The coordinator opens permit join for 180 s on boot. Or send from the backend:

```json
{"cmd":"PERMIT_JOIN","duration":180}
```

## Protocol (Pi ↔ coordinator)

One JSON object per line over USB @ 115200 baud.

Downstream:

```json
{"cmd":"TURN_LED_ON","ieee":"0x1051dbfffe1c4a78"}
{"cmd":"TURN_LED_OFF","ieee":"0x1051dbfffe1c4a78"}
{"cmd":"BLINK_LED","ieee":"0x1051dbfffe1c4a78","duration_ms":3000}
{"cmd":"PERMIT_JOIN","duration":180}
```

Upstream:

```json
{"event":"NODE_ONLINE","ieee":"0x1051dbfffe1c4a78","short":"0x3b4c"}
{"event":"BUTTON_PRESSED","ieee":"0x1051dbfffe1c4a78"}
{"event":"HEARTBEAT","ieee":"0x1051dbfffe1c4a78","seq":42}
{"event":"LED_STATE","ieee":"0x1051dbfffe1c4a78","state":1}
```

See `docs/architecture/put-to-light-architecture.md` for full design.

## Legacy Zigbee2MQTT path

`gateway/zigbee2mqtt/` remains for bench setups with a SONOFF dongle. Use `SHELF_BACKEND=mqtt ./scripts/run_web_master.sh` — not used in the ESP32-coordinator production path.

## Repo layout

- `firmware/coordinator/` — Zigbee coordinator + JSON USB gateway
- `firmware/shelf_node/` — HA On/Off light + button + custom events cluster
- `components/shelf_protocol/` — shared message constants + JSON helpers
- `gateway/web_app/` — FastAPI UI (`--backend serial` default)
- `docs/architecture/put-to-light-architecture.md` — system design
