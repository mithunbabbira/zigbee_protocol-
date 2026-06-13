/*
 * Shelf Zigbee Router node — HA On/Off light with BOOT-button upstream reporting.
 */
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "alarm_timer.h"
#include "status_led.h"
#include "switch_driver.h"

#include "esp_zigbee.h"
#include "ezbee/zha.h"

#include "shelf_node.h"
#include "shelf_zigbee.h"

static const char *TAG = "SHELF_NODE";

static uint8_t s_shelf_state;

static bool esp_zigbee_app_signal_handler(const ezb_app_signal_t *app_signal);

static void shelf_report_state_upstream(void)
{
    ezb_err_t ret;
    ezb_zcl_report_attr_cmd_t report_attr_cmd = {
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
    ret = ezb_zcl_report_attr_cmd_req(&report_attr_cmd);
    esp_zigbee_lock_release();

    if (ret == EZB_ERR_NONE) {
        ESP_LOGI(TAG, "Reported state %u to coordinator", s_shelf_state);
    } else {
        ESP_LOGE(TAG, "Failed to report state, error 0x%04x", ret);
    }
}

static void shelf_apply_local_state(uint8_t state, const char *reason)
{
    s_shelf_state = state ? 1U : 0U;
    status_led_set(s_shelf_state != 0U);
    ESP_LOGI(TAG, "SHELF_LOCAL_STATE:%u%s", s_shelf_state, reason);

    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_zcl_set_attr_value(SHELF_HA_LIGHT_EP_ID, EZB_ZCL_CLUSTER_ID_ON_OFF, EZB_ZCL_CLUSTER_SERVER,
                           EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, EZB_ZCL_STD_MANUF_CODE, &s_shelf_state, false);
    esp_zigbee_lock_release();
}

static void shelf_toggle_local_state(void)
{
    shelf_apply_local_state(s_shelf_state ? 0U : 1U, "");
    shelf_report_state_upstream();
}

static void button_event_handler(switch_driver_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle != SWITCH_INV_HANDLE, , TAG, "Invalid switch handle");
    shelf_toggle_local_state();
}

static esp_err_t deferred_driver_init(void)
{
    static bool is_inited = false;

    ESP_RETURN_ON_FALSE(!is_inited, ESP_OK, TAG, "Deferred driver already initialized");

    switch_driver_config_t config = {
        .gpio_num = CONFIG_GPIO_BOOT_ON_DEVKIT,
        .event_cb = button_event_handler,
    };

    ESP_RETURN_ON_FALSE(switch_driver_init(&config) != SWITCH_INV_HANDLE, ESP_FAIL, TAG,
                        "Failed to initialize BOOT button driver");
    ESP_RETURN_ON_ERROR(status_led_init(), TAG, "Failed to initialize status LED");
    is_inited = true;
    return ESP_OK;
}

static void esp_zigbee_alarm_bdb_commissioning(alarm_timer_arg_t arg)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    (void)ezb_bdb_start_top_level_commissioning(arg);
    esp_zigbee_lock_release();
}

static void zcl_core_set_attr_value_handler(ezb_zcl_set_attr_value_message_t *message)
{
    ESP_RETURN_ON_FALSE(message, , TAG, "Empty set attribute message");
    if (message->info.cluster_id != EZB_ZCL_CLUSTER_ID_ON_OFF) {
        return;
    }

    shelf_apply_local_state(*(uint8_t *)message->in.attribute.data.value ? 1U : 0U, " (downstream)");
}

static void esp_zigbee_zcl_core_action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    switch (callback_id) {
    case EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID:
        zcl_core_set_attr_value_handler(message);
        break;
    case EZB_ZCL_CORE_DEFAULT_RSP_CB_ID: {
        ezb_zcl_cmd_default_rsp_message_t *default_rsp = message;
        ESP_LOGD(TAG, "ZCL default response status 0x%02x", default_rsp->in.status_code);
    } break;
    default:
        ESP_LOGD(TAG, "Unhandled ZCL action 0x%08lx", callback_id);
        break;
    }
}

static esp_err_t shelf_create_ha_on_off_light_device(void)
{
    ezb_af_device_desc_t dev_desc = ezb_af_create_device_desc();
    ezb_zha_on_off_light_config_t light_cfg = EZB_ZHA_ON_OFF_LIGHT_CONFIG();
    ezb_af_ep_desc_t ep_desc = ezb_zha_create_on_off_light(SHELF_HA_LIGHT_EP_ID, &light_cfg);
    ezb_zcl_cluster_desc_t basic_desc =
        ezb_af_endpoint_get_cluster_desc(ep_desc, EZB_ZCL_CLUSTER_ID_BASIC, EZB_ZCL_CLUSTER_SERVER);

    ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
                                        (void *)SHELF_MANUFACTURER_NAME);
    ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
                                        (void *)SHELF_MODEL_IDENTIFIER);

    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(dev_desc, ep_desc));
    ESP_ERROR_CHECK(ezb_af_device_desc_register(dev_desc));
    ezb_zcl_core_action_handler_register(esp_zigbee_zcl_core_action_handler);

    s_shelf_state = 0U;
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_zcl_set_attr_value(SHELF_HA_LIGHT_EP_ID, EZB_ZCL_CLUSTER_ID_ON_OFF, EZB_ZCL_CLUSTER_SERVER,
                           EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, EZB_ZCL_STD_MANUF_CODE, &s_shelf_state, false);
    esp_zigbee_lock_release();

    return ESP_OK;
}

static esp_err_t shelf_setup_commissioning(void)
{
    ezb_aps_secur_enable_distributed_security(false);
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(SHELF_PRIMARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_bdb_set_secondary_channel_set(SHELF_SECONDARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(esp_zigbee_app_signal_handler));
    return ESP_OK;
}

static bool esp_zigbee_app_signal_handler(const ezb_app_signal_t *app_signal)
{
    ezb_app_signal_type_t signal_type = ezb_app_signal_get_type(app_signal);

    switch (signal_type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Deferred driver init %s", deferred_driver_init() == ESP_OK ? "ok" : "failed");
            ESP_LOGI(TAG, "Started in%s factory-reset mode", ezb_bdb_is_factory_new() ? "" : " non");
            if (ezb_bdb_is_factory_new()) {
                ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGI(TAG, "Device reboot, rejoining network");
                ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
            }
        } else {
            ESP_LOGW(TAG, "%s failed (0x%02x), retrying", ezb_app_signal_to_string(signal_type), status);
            alarm_timer_schedule(esp_zigbee_alarm_bdb_commissioning, EZB_BDB_MODE_INITIALIZATION, 1000);
        }
    } break;
    case EZB_BDB_SIGNAL_STEERING: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ezb_extpanid_t extended_pan_id;
            ezb_nwk_get_extended_panid(&extended_pan_id);
            ESP_LOGI(TAG, "Joined network: PAN 0x%04hx, channel %d, short addr 0x%04hx",
                     ezb_nwk_get_panid(), ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
        } else {
            ESP_LOGW(TAG, "Network steering failed (0x%02x), retrying", status);
            alarm_timer_schedule(esp_zigbee_alarm_bdb_commissioning, EZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
    } break;
    case EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS: {
        uint8_t duration = *(uint8_t *)ezb_app_signal_get_params(app_signal);
        ESP_LOGI(TAG, "Permit join %u seconds on PAN 0x%04hx", duration, ezb_nwk_get_panid());
    } break;
    default:
        ESP_LOGD(TAG, "Signal %s (0x%02x)", ezb_app_signal_to_string(signal_type), signal_type);
        break;
    }

    return true;
}

static void esp_zigbee_stack_main_task(void *pvParameters)
{
    esp_zigbee_config_t config = SHELF_ZIGBEE_DEFAULT_CONFIG();

    ESP_ERROR_CHECK(esp_zigbee_init(&config));
    ESP_ERROR_CHECK(shelf_zigbee_apply_router_memory());
    ESP_ERROR_CHECK(shelf_setup_commissioning());
    ESP_ERROR_CHECK(shelf_create_ha_on_off_light_device());
    ESP_ERROR_CHECK(esp_zigbee_start(false));

    esp_zigbee_launch_mainloop();
    esp_zigbee_deinit();
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(nvs_flash_init_partition(SHELF_ZIGBEE_STORAGE_PARTITION_NAME));
    ESP_LOGI(TAG, "Shelf node starting (Zigbee Router)");
    xTaskCreate(esp_zigbee_stack_main_task, "Zigbee_main", 4096, NULL, 5, NULL);
}
