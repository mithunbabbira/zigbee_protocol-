#include "esp_check.h"
#include "esp_log.h"

#include "button_controller.h"

static const char *TAG = "BTN_CTRL";
static button_controller_cb_t s_press_cb;

static void button_event_handler(switch_driver_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle != SWITCH_INV_HANDLE, , TAG, "Invalid switch handle");
    if (s_press_cb != NULL) {
        s_press_cb();
    }
}

esp_err_t button_controller_init(button_controller_cb_t cb)
{
    s_press_cb = cb;

    switch_driver_config_t config = {
        .gpio_num = CONFIG_GPIO_BOOT_ON_DEVKIT,
        .event_cb = button_event_handler,
    };

    ESP_RETURN_ON_FALSE(switch_driver_init(&config) != SWITCH_INV_HANDLE, ESP_FAIL, TAG,
                        "Failed to initialize button driver");
    return ESP_OK;
}
