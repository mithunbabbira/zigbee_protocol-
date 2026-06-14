/*
 * Put-to-Light Zigbee coordinator — thin app entry.
 */
#include "esp_log.h"
#include "nvs_flash.h"

#include "gateway_transport.h"
#include "message_handler.h"
#include "shelf_zigbee.h"
#include "zigbee_coordinator.h"

static const char *TAG = "COORDINATOR";

static void gateway_line_cb(const char *line, void *user_ctx)
{
    (void)user_ctx;
    message_handler_on_gateway_line(line);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(nvs_flash_init_partition(SHELF_ZIGBEE_STORAGE_PARTITION_NAME));

    ESP_LOGI(TAG, "Coordinator starting (JSON gateway + Zigbee ZC)");

    ESP_ERROR_CHECK(zigbee_coordinator_init());
    message_handler_init();
    ESP_ERROR_CHECK(gateway_transport_console_init());
    gateway_transport_init(gateway_line_cb, NULL);
    gateway_transport_start();
    zigbee_coordinator_start_task();
}
