#!/usr/bin/env bash
# Bench validation helper for the 3-board wireless loop.
set -euo pipefail

cat <<'EOF'
Shelf Zigbee 3-board bench checklist
==================================

Prerequisites
- ESP-IDF 5.2+ with esp-zigbee-lib 2.x
- 3x ESP32-C6 dev boards
- Build/flash:
    source $IDF_PATH/export.sh
    ./scripts/build_all.sh
    cd firmware/coordinator && idf.py -p PORT flash monitor
    cd firmware/shelf_node && idf.py -p PORT flash monitor

Steps
1. Network form — coordinator logs PAN ID, channel, short addr 0x0000
2. Join — each shelf_node logs its short address after steering
3. Bind/provision — coordinator logs "Binding shelf" and "Added shelf ... to group 0x0001"
4. Downstream unicast — send: NODE_SEND:0xXXXX:STATE:1 (use shelf short addr)
5. Downstream groupcast — send: GROUP_SEND:0x0001:STATE:0
6. Upstream BOOT — press BOOT on a shelf; coordinator prints NODE_RECV:0xXXXX:STATE:1
7. Collision spot-check — rapid BOOT on two shelves; both NODE_RECV lines should appear <1s

Downstream commands (coordinator USB serial):
  NODE_SEND:0x2638:STATE:1
  GROUP_SEND:0x0001:STATE:0

Expected upstream line:
  NODE_RECV:0x2638:STATE:1
EOF
