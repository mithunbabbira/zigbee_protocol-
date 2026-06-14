#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "alarm_timer.h"
#include "button_controller.h"
#include "esp_zigbee.h"
#include "ezbee/zcl/cluster/custom.h"
#include "ezbee/zha.h"
#include "led_controller.h"
#include "shelf_node.h"
#include "shelf_protocol.h"
#include "shelf_zigbee.h"
#include "zigbee_shelf_node.h"

static const char *TAG = "ZB_SHELF";

static uint8_t s_led_state;
static uint32_t s_heartbeat_seq;
static bool s_network_joined;

static bool esp_zigbee_app_signal_handler(const ezb_app_signal_t *app_signal);

static void shelf_report_on_off_upstream(void)
{
    ezb_zcl_report_attr_cmd_t report = {
        .cmd_ctrl =
            {
                .fc.direction       = EZB_ZCL_CMD_DIRECTION_TO_CLI,
                .dst_addr.addr_mode = EZB_ADDR_MODE_SHORT,
                .dst_addr.u.short_addr = SHELF_COORDINATOR_SHORT_ADDR,
                .dst_ep             = SHELF_HA_GATEWAY_EP_ID,
                .src_ep             = SHELF_HA_LIGHT_EP_ID,
                .cluster_id         = EZB_ZCL_CLUSTER_ID_ON_OFF,
            },
        .payload =
            {
                .attr_id = EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
            },
    };

    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_err_t ret = ezb_zcl_report_attr_cmd_req(&report);
    esp_zigbee_lock_release();

    if (ret != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "On/Off report failed 0x%04x", ret);
    }
}

static void shelf_apply_led_state(uint8_t state, const char *reason)
{
    s_led_state = state ? 1U : 0U;
    led_controller_set(s_led_state != 0U);
    ESP_LOGI(TAG, "LED %s%s", s_led_state ? "ON" : "OFF", reason != NULL ? reason : "");

    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_zcl_set_attr_value(SHELF_HA_LIGHT_EP_ID, EZB_ZCL_CLUSTER_ID_ON_OFF, EZB_ZCL_CLUSTER_SERVER,
                           EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, EZB_ZCL_STD_MANUF_CODE, &s_led_state, false);
    esp_zigbee_lock_release();
}

static void shelf_send_button_pressed(void)
{
    ezb_zcl_custom_cluster_cmd_t cmd = {
        .cmd_ctrl =
            {
                .fc.direction       = EZB_ZCL_CMD_DIRECTION_TO_CLI,
                .dst_addr.addr_mode = EZB_ADDR_MODE_SHORT,
                .dst_addr.u.short_addr = SHELF_COORDINATOR_SHORT_ADDR,
                .dst_ep             = SHELF_HA_GATEWAY_EP_ID,
                .src_ep             = SHELF_HA_LIGHT_EP_ID,
                .cluster_id         = SHELF_CLUSTER_EVENTS_ID,
            },
        .cmd_id      = SHELF_CMD_BUTTON_PRESSED_ID,
        .data_length = 0,
        .data        = NULL,
    };

    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_err_t ret = ezb_zcl_custom_cluster_cmd_req(&cmd);
    esp_zigbee_lock_release();

    if (ret == EZB_ERR_NONE) {
        ESP_LOGI(TAG, "Sent BUTTON_PRESSED");
    } else {
        ESP_LOGW(TAG, "BUTTON_PRESSED failed 0x%04x", ret);
    }
}

static void shelf_send_heartbeat(void)
{
    s_heartbeat_seq++;

    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_zcl_set_attr_value(SHELF_HA_LIGHT_EP_ID, SHELF_CLUSTER_EVENTS_ID, EZB_ZCL_CLUSTER_SERVER,
                           SHELF_ATTR_HEARTBEAT_SEQ_ID, EZB_ZCL_STD_MANUF_CODE, &s_heartbeat_seq, false);

    ezb_zcl_report_attr_cmd_t report = {
        .cmd_ctrl =
            {
                .fc.direction       = EZB_ZCL_CMD_DIRECTION_TO_CLI,
                .dst_addr.addr_mode = EZB_ADDR_MODE_SHORT,
                .dst_addr.u.short_addr = SHELF_COORDINATOR_SHORT_ADDR,
                .dst_ep             = SHELF_HA_GATEWAY_EP_ID,
                .src_ep             = SHELF_HA_LIGHT_EP_ID,
                .cluster_id         = SHELF_CLUSTER_EVENTS_ID,
            },
        .payload =
            {
                .attr_id = SHELF_ATTR_HEARTBEAT_SEQ_ID,
            },
    };
    ezb_err_t ret = ezb_zcl_report_attr_cmd_req(&report);
    esp_zigbee_lock_release();

    if (ret == EZB_ERR_NONE) {
        ESP_LOGD(TAG, "Heartbeat seq=%lu", (unsigned long)s_heartbeat_seq);
    }
}

static void heartbeat_alarm(alarm_timer_arg_t arg)
{
    (void)arg;
    if (s_network_joined) {
        shelf_send_heartbeat();
    }
    alarm_timer_schedule(heartbeat_alarm, 0, 30000);
}

void zigbee_shelf_node_on_button_pressed(void)
{
    shelf_apply_led_state(s_led_state ? 0U : 1U, " (button)");
    shelf_report_on_off_upstream();
    shelf_send_button_pressed();
}

static void zcl_set_attr_value_handler(ezb_zcl_set_attr_value_message_t *message)
{
    ESP_RETURN_ON_FALSE(message, , TAG, "Empty set attribute message");
    if (message->info.cluster_id != EZB_ZCL_CLUSTER_ID_ON_OFF) {
        return;
    }

    shelf_apply_led_state(*(uint8_t *)message->in.attribute.data.value ? 1U : 0U, " (downstream)");
    shelf_report_on_off_upstream();
}

static void esp_zigbee_zcl_core_action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    if (callback_id == EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID) {
        zcl_set_attr_value_handler(message);
    }
}

static esp_err_t zigbee_shelf_node_create_device(void)
{
    static uint32_t heartbeat_attr = 0;

    ezb_af_device_desc_t dev_desc = ezb_af_create_device_desc();
    ezb_zha_on_off_light_config_t light_cfg = EZB_ZHA_ON_OFF_LIGHT_CONFIG();
    ezb_af_ep_desc_t ep_desc = ezb_zha_create_on_off_light(SHELF_HA_LIGHT_EP_ID, &light_cfg);

    ezb_zcl_cluster_desc_t basic_desc =
        ezb_af_endpoint_get_cluster_desc(ep_desc, EZB_ZCL_CLUSTER_ID_BASIC, EZB_ZCL_CLUSTER_SERVER);
    ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
                                        (void *)SHELF_MANUFACTURER_NAME);
    ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
                                        (void *)SHELF_MODEL_IDENTIFIER);

    ezb_zcl_custom_cluster_config_t events_cfg = {
        .cluster_id = SHELF_CLUSTER_EVENTS_ID,
    };
    ezb_zcl_cluster_desc_t events_server =
        ezb_zcl_custom_create_cluster_desc(&events_cfg, EZB_ZCL_CLUSTER_SERVER);
    ESP_ERROR_CHECK(ezb_zcl_custom_cluster_desc_add_attr(events_server, SHELF_ATTR_HEARTBEAT_SEQ_ID,
                                                         EZB_ZCL_ATTR_TYPE_UINT32,
                                                         EZB_ZCL_ATTR_ACCESS_READ_ONLY | EZB_ZCL_ATTR_ACCESS_REPORTING,
                                                         &heartbeat_attr));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep_desc, events_server));

    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(dev_desc, ep_desc));
    ESP_ERROR_CHECK(ezb_af_device_desc_register(dev_desc));
    ezb_zcl_core_action_handler_register(esp_zigbee_zcl_core_action_handler);

    s_led_state = 0U;
    shelf_apply_led_state(0U, " (init)");
    return ESP_OK;
}

static void esp_zigbee_alarm_bdb_commissioning(alarm_timer_arg_t arg)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    (void)ezb_bdb_start_top_level_commissioning(arg);
    esp_zigbee_lock_release();
}

static bool esp_zigbee_app_signal_handler(const ezb_app_signal_t *app_signal)
{
    ezb_app_signal_type_t signal_type = ezb_app_signal_get_type(app_signal);

    switch (signal_type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
        } else {
            alarm_timer_schedule(esp_zigbee_alarm_bdb_commissioning, EZB_BDB_MODE_INITIALIZATION, 1000);
        }
    } break;
    case EZB_BDB_SIGNAL_STEERING: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            s_network_joined = true;
            ESP_LOGI(TAG, "Joined network PAN 0x%04hx channel %d short 0x%04hx", ezb_nwk_get_panid(),
                     ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
            alarm_timer_schedule(heartbeat_alarm, 0, 30000);
        } else {
            alarm_timer_schedule(esp_zigbee_alarm_bdb_commissioning, EZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
    } break;
    default:
        break;
    }

    return true;
}

static void zigbee_shelf_node_stack_task(void *arg)
{
    (void)arg;

    esp_zigbee_config_t config = SHELF_ZIGBEE_DEFAULT_CONFIG();

    ESP_ERROR_CHECK(esp_zigbee_init(&config));
#if CONFIG_SHELF_DEVICE_ROLE_ROUTER
    ESP_ERROR_CHECK(shelf_zigbee_apply_router_memory());
#else
    ESP_ERROR_CHECK(shelf_zigbee_apply_end_device_memory());
#endif

    ezb_aps_secur_enable_distributed_security(false);
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(SHELF_PRIMARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_bdb_set_secondary_channel_set(SHELF_SECONDARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(esp_zigbee_app_signal_handler));
    ESP_ERROR_CHECK(zigbee_shelf_node_create_device());
    ESP_ERROR_CHECK(esp_zigbee_start(false));

    esp_zigbee_launch_mainloop();
    esp_zigbee_deinit();
    vTaskDelete(NULL);
}

esp_err_t zigbee_shelf_node_init(void)
{
    ESP_ERROR_CHECK(led_controller_init());
    ESP_ERROR_CHECK(button_controller_init(zigbee_shelf_node_on_button_pressed));
    return ESP_OK;
}

void zigbee_shelf_node_start_task(void)
{
    xTaskCreate(zigbee_shelf_node_stack_task, "Zigbee_main", 4096, NULL, 5, NULL);
}
