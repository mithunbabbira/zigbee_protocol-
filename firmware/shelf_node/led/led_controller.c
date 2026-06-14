#include "esp_err.h"
#include "status_led.h"

#include "led_controller.h"

esp_err_t led_controller_init(void)
{
    return status_led_init();
}

void led_controller_set(bool on)
{
    status_led_set(on);
}

void led_controller_blink(uint32_t duration_ms)
{
    (void)duration_ms;
    led_controller_set(true);
}
