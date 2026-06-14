idf_component_register(
    SRC_DIRS "." "../zigbee" "../communication" "../events"
    INCLUDE_DIRS "." "../zigbee" "../communication" "../events"
    PRIV_REQUIRES vfs nvs_flash driver esp-zigbee-lib shelf_zigbee shelf_protocol alarm_timer)
