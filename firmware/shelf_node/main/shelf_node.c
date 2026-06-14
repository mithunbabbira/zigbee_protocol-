/*
 * Put-to-Light shelf node — thin app entry.
 */
#include "esp_log.h"
#include "nvs_flash.h"

#include "shelf_node.h"
#include "shelf_zigbee.h"
#include "zigbee_shelf_node.h"

static const char *TAG = "SHELF_NODE";

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(nvs_flash_init_partition(SHELF_ZIGBEE_STORAGE_PARTITION_NAME));

#if CONFIG_SHELF_DEVICE_ROLE_ROUTER
    ESP_LOGI(TAG, "Shelf node starting (Zigbee Router)");
#else
    ESP_LOGI(TAG, "Shelf node starting (Zigbee End Device)");
#endif

    ESP_ERROR_CHECK(zigbee_shelf_node_init());
    zigbee_shelf_node_start_task();
}
