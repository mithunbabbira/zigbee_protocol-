#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_dev.h"
#include "esp_vfs_usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gateway_transport.h"
#include "shelf_protocol.h"

static const char *TAG = "GW_TRANSPORT";

static gateway_transport_line_cb_t s_line_cb;
static void *s_line_ctx;

esp_err_t gateway_transport_console_init(void)
{
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    esp_vfs_dev_usb_serial_jtag_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
    esp_vfs_dev_usb_serial_jtag_set_tx_line_endings(ESP_LINE_ENDINGS_LF);

    usb_serial_jtag_driver_config_t usb_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(usb_serial_jtag_driver_install(&usb_cfg), TAG, "USB driver install failed");
    esp_vfs_usb_serial_jtag_use_driver();

    fcntl(fileno(stdin), F_SETFL, O_NONBLOCK);
    fcntl(fileno(stdout), F_SETFL, O_NONBLOCK);
    return ESP_OK;
}

void gateway_transport_init(gateway_transport_line_cb_t cb, void *user_ctx)
{
    s_line_cb = cb;
    s_line_ctx = user_ctx;
}

void gateway_transport_emit(const char *json_line)
{
    if (json_line == NULL) {
        return;
    }

    fputs(json_line, stdout);
    if (json_line[strlen(json_line) - 1U] != '\n') {
        fputc('\n', stdout);
    }
    fflush(stdout);
}

static void gateway_transport_task(void *arg)
{
    (void)arg;

    char line[SHELF_PROTOCOL_LINE_MAX];
    size_t pos = 0;

    ESP_LOGI(TAG, "JSON gateway ready");

    while (true) {
        int ch = fgetc(stdin);
        if (ch == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (ch == '\n' || ch == '\r') {
            if (pos > 0U) {
                line[pos] = '\0';
                if (s_line_cb != NULL) {
                    s_line_cb(line, s_line_ctx);
                }
                pos = 0;
            }
            continue;
        }

        if (pos + 1U < sizeof(line)) {
            line[pos++] = (char)ch;
        }
    }
}

void gateway_transport_start(void)
{
    xTaskCreate(gateway_transport_task, "gw_transport", 4096, NULL, 4, NULL);
}
