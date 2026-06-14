idf_component_register(
    SRC_DIRS "." "../zigbee" "../led" "../button"
    INCLUDE_DIRS "." "../zigbee" "../led" "../button"
    PRIV_REQUIRES vfs nvs_flash esp-zigbee-lib shelf_zigbee shelf_protocol switch_driver status_led alarm_timer)
