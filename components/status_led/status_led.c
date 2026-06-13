#include "status_led.h"

#include "esp_check.h"
#include "esp_log.h"
#include "led_strip.h"

static const char *TAG = "STATUS_LED";

static led_strip_handle_t s_led_strip;
static bool s_initialized;

esp_err_t status_led_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    led_strip_config_t strip_cfg = {
        .strip_gpio_num = CONFIG_GPIO_STATUS_LED_ON_DEVKIT,
        .max_leds       = 1,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,
    };

    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_led_strip), TAG,
                        "Failed to init status LED strip");
    s_initialized = true;
    status_led_set(false);
    ESP_LOGI(TAG, "Status LED ready on GPIO %d", CONFIG_GPIO_STATUS_LED_ON_DEVKIT);
    return ESP_OK;
}

void status_led_set(bool on)
{
    if (!s_initialized) {
        return;
    }

    uint8_t level = on ? 32U : 0U;
    ESP_ERROR_CHECK(led_strip_set_pixel(s_led_strip, 0, 0, level, 0));
    ESP_ERROR_CHECK(led_strip_refresh(s_led_strip));
}
