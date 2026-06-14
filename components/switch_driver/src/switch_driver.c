#include <stdlib.h>
#include <string.h>

#include "switch_driver.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static QueueHandle_t gpio_event_queue = NULL;

static void switch_driver_gpio_intr_enabled(gpio_num_t gpio_num, bool enabled)
{
    if (enabled) {
        gpio_intr_enable(gpio_num);
    } else {
        gpio_intr_disable(gpio_num);
    }
}

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    switch_driver_config_t *config = (switch_driver_config_t *)arg;
    switch_driver_gpio_intr_enabled(config->gpio_num, false);
    xQueueSendFromISR(gpio_event_queue, &config, NULL);
}

static void switch_driver_button_detected(void *arg)
{
    (void)arg;
    switch_driver_config_t *config = NULL;

    while (1) {
        if (xQueueReceive(gpio_event_queue, &config, portMAX_DELAY) == pdTRUE) {
            if (config && config->event_cb) {
                config->event_cb(config);
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            if (config) {
                switch_driver_gpio_intr_enabled(config->gpio_num, true);
            }
        }
    }
}

switch_driver_handle_t switch_driver_init(const switch_driver_config_t *config)
{
    if (config == NULL || config->gpio_num < 0) {
        return NULL;
    }

    switch_driver_config_t *handle = (switch_driver_config_t *)malloc(sizeof(switch_driver_config_t));
    if (handle == NULL) {
        return NULL;
    }
    memcpy(handle, config, sizeof(switch_driver_config_t));

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << config->gpio_num),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };

    if (gpio_config(&io_conf) != ESP_OK) {
        free(handle);
        return NULL;
    }

    if (gpio_event_queue == NULL) {
        gpio_event_queue = xQueueCreate(10, sizeof(switch_driver_config_t *));
        if (gpio_event_queue == NULL) {
            gpio_reset_pin(config->gpio_num);
            free(handle);
            return NULL;
        }
        xTaskCreate(switch_driver_button_detected, "button_detected", 4096, NULL, 10, NULL);
    }

    esp_err_t isr_ret = gpio_install_isr_service(0);
    if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
        gpio_reset_pin(config->gpio_num);
        free(handle);
        return NULL;
    }

    if (gpio_isr_handler_add(config->gpio_num, gpio_isr_handler, handle) != ESP_OK) {
        gpio_reset_pin(config->gpio_num);
        free(handle);
        return NULL;
    }

    return handle;
}

void switch_driver_deinit(switch_driver_handle_t handle)
{
    if (handle == NULL) {
        return;
    }

    switch_driver_config_t *config = (switch_driver_config_t *)handle;
    gpio_isr_handler_remove(config->gpio_num);
    gpio_reset_pin(config->gpio_num);
    free(config);
}
