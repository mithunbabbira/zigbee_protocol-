#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "alarm_timer.h"
#include "device_registry.h"
#include "esp_zigbee.h"
#include "ezbee/nwk.h"
#include "ezbee/zcl/cluster/custom.h"
#include "ezbee/zcl/cluster/identify.h"
#include "ezbee/zha.h"
#include "message_handler.h"
#include "shelf_protocol.h"
#include "shelf_zigbee.h"
#include "zigbee_coordinator.h"

static const char *TAG = "ZB_COORD";

static bool esp_zigbee_app_signal_handler(const ezb_app_signal_t *app_signal);

static void ieee_bytes_from_extaddr(const ezb_extaddr_t *ext, uint8_t ieee[8])
{
    memcpy(ieee, ext->u8, 8);
}

static void extaddr_from_ieee_bytes(ezb_extaddr_t *ext, const uint8_t ieee[8])
{
    memcpy(ext->u8, ieee, 8);
}

static void zigbee_coordinator_send_on_off_short(uint16_t short_addr, bool on)
{
    ezb_zcl_on_off_cmd_t cmd_req = {
        .cmd_ctrl =
            {
                .dst_addr.addr_mode = EZB_ADDR_MODE_SHORT,
                .dst_addr.u.short_addr = short_addr,
                .dst_ep             = SHELF_HA_LIGHT_EP_ID,
                .src_ep             = SHELF_HA_GATEWAY_EP_ID,
            },
    };

    esp_zigbee_lock_acquire(portMAX_DELAY);
    if (on) {
        ezb_zcl_on_off_on_cmd_req(&cmd_req);
    } else {
        ezb_zcl_on_off_off_cmd_req(&cmd_req);
    }
    esp_zigbee_lock_release();
}

void zigbee_coordinator_send_on_off(const uint8_t ieee[8], bool on)
{
    uint16_t short_addr = 0;
    if (!device_registry_resolve_ieee_to_short(ieee, &short_addr)) {
        ezb_extaddr_t ext_addr;
        extaddr_from_ieee_bytes(&ext_addr, ieee);
        if (ezb_address_short_by_extended(&ext_addr, &short_addr) != EZB_ERR_NONE) {
            ESP_LOGW(TAG, "Unknown IEEE for on/off command");
            return;
        }
    }

    zigbee_coordinator_send_on_off_short(short_addr, on);
    ESP_LOGI(TAG, "Sent %s to 0x%04hx", on ? "ON" : "OFF", short_addr);
}

void zigbee_coordinator_send_blink(const uint8_t ieee[8], uint32_t duration_ms)
{
    uint16_t short_addr = 0;
    if (!device_registry_resolve_ieee_to_short(ieee, &short_addr)) {
        ezb_extaddr_t ext_addr;
        extaddr_from_ieee_bytes(&ext_addr, ieee);
        if (ezb_address_short_by_extended(&ext_addr, &short_addr) != EZB_ERR_NONE) {
            ESP_LOGW(TAG, "Unknown IEEE for blink command");
            return;
        }
    }

    ezb_zcl_identify_trigger_effect_cmd_t cmd = {
        .cmd_ctrl =
            {
                .dst_addr.addr_mode = EZB_ADDR_MODE_SHORT,
                .dst_addr.u.short_addr = short_addr,
                .dst_ep             = SHELF_HA_LIGHT_EP_ID,
                .src_ep             = SHELF_HA_GATEWAY_EP_ID,
            },
        .payload =
            {
                .effect_id     = 0x00,
                .effect_variant = 0,
            },
    };

    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_zcl_identify_trigger_effect_cmd_req(&cmd);
    esp_zigbee_lock_release();
    ESP_LOGI(TAG, "Sent BLINK to 0x%04hx (%lu ms)", short_addr, (unsigned long)duration_ms);
}

void zigbee_coordinator_open_network(uint16_t duration_sec)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_bdb_open_network(duration_sec);
    esp_zigbee_lock_release();
    ESP_LOGI(TAG, "Permit join opened for %u seconds", duration_sec);
}

static void zdo_bind_shelf_device_result(const ezb_zdp_bind_req_result_t *result, void *user_ctx)
{
    uint16_t short_addr = (uint16_t)(uintptr_t)user_ctx;
    if (result == NULL) {
        return;
    }

    if (result->error == EZB_ERR_NONE && result->rsp && result->rsp->status == EZB_ZDP_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "Bound shelf 0x%04hx", short_addr);
    } else {
        ESP_LOGW(TAG, "Bind failed for 0x%04hx", short_addr);
    }
}

static ezb_err_t zigbee_coordinator_bind_shelf(uint16_t dst_short_addr, uint8_t dst_ep)
{
    ezb_zdo_bind_req_t bind_req = {
        .dst_nwk_addr = ezb_nwk_get_short_address(),
        .field =
            {
                .src_ep        = SHELF_HA_GATEWAY_EP_ID,
                .cluster_id    = EZB_ZCL_CLUSTER_ID_ON_OFF,
                .dst_addr_mode = EZB_ADDR_MODE_EXT,
                .dst_ep        = dst_ep,
            },
        .cb       = zdo_bind_shelf_device_result,
        .user_ctx = (void *)(uintptr_t)dst_short_addr,
    };

    ezb_nwk_get_extended_address(&bind_req.field.src_addr);
    ESP_RETURN_ON_ERROR(ezb_address_extended_by_short(dst_short_addr, &bind_req.field.dst_addr.extended_addr), TAG,
                        "Resolve ext addr failed");

    return ezb_zdo_bind_req(&bind_req);
}

static void zdo_find_shelf_device_result(const ezb_zdo_match_desc_req_result_t *result, void *user_ctx)
{
    (void)user_ctx;
    if (result == NULL || result->error != EZB_ERR_NONE || result->rsp == NULL) {
        return;
    }

    if (result->rsp->status != EZB_ZDP_STATUS_SUCCESS || result->rsp->match_length == 0 || result->rsp->match_list == NULL) {
        return;
    }

    for (size_t i = 0; i < result->rsp->match_length; i++) {
        zigbee_coordinator_bind_shelf(result->rsp->nwk_addr_of_interest, result->rsp->match_list[i]);
    }
}

static void zigbee_coordinator_provision_shelf(uint16_t short_addr)
{
    uint16_t cluster_list[] = {EZB_ZCL_CLUSTER_ID_ON_OFF};
    ezb_zdo_match_desc_req_t req = {
        .dst_nwk_addr = short_addr,
        .field =
            {
                .nwk_addr_of_interest = short_addr,
                .profile_id           = EZB_AF_HA_PROFILE_ID,
                .num_in_clusters      = 1,
                .num_out_clusters     = 0,
                .cluster_list         = cluster_list,
            },
        .cb       = zdo_find_shelf_device_result,
        .user_ctx = NULL,
    };

    if (ezb_zdo_match_desc_req(&req) == EZB_ERR_NONE) {
        ESP_LOGI(TAG, "Provisioning shelf 0x%04hx", short_addr);
    }
}

static void zigbee_coordinator_register_joined_device(uint16_t short_addr)
{
    ezb_extaddr_t ext_addr;
    if (ezb_address_extended_by_short(short_addr, &ext_addr) != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "Could not resolve IEEE for 0x%04hx", short_addr);
        return;
    }

    uint8_t ieee[8];
    ieee_bytes_from_extaddr(&ext_addr, ieee);
    device_registry_upsert(ieee, short_addr);
    zigbee_coordinator_provision_shelf(short_addr);
}

static void zcl_report_attr_handler(ezb_zcl_cmd_report_attr_message_t *message)
{
    ESP_RETURN_ON_FALSE(message && message->in.header, , TAG, "Empty report");

    uint16_t src_short = message->in.header->src_addr.u.short_addr;
    ezb_extaddr_t ext_addr;
    if (ezb_address_extended_by_short(src_short, &ext_addr) != EZB_ERR_NONE) {
        return;
    }

    uint8_t ieee[8];
    ieee_bytes_from_extaddr(&ext_addr, ieee);

    if (message->info.cluster_id == EZB_ZCL_CLUSTER_ID_ON_OFF) {
        for (ezb_zcl_report_attr_variable_t *var = message->in.variables; var != NULL; var = var->next) {
            if (var->attr_id == EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID && var->attr_type == EZB_ZCL_ATTR_TYPE_BOOL &&
                var->attr_value != NULL) {
                uint8_t state = (*(uint8_t *)var->attr_value) ? 1U : 0U;
                message_handler_emit_led_state(ieee, state);
            }
        }
        return;
    }

    if (message->info.cluster_id == SHELF_CLUSTER_EVENTS_ID) {
        for (ezb_zcl_report_attr_variable_t *var = message->in.variables; var != NULL; var = var->next) {
            if (var->attr_id == SHELF_ATTR_HEARTBEAT_SEQ_ID && var->attr_value != NULL) {
                uint32_t seq = *(uint32_t *)var->attr_value;
                device_registry_mark_heartbeat(ieee, seq);
            }
        }
    }
}

static ezb_zcl_status_t coord_events_process_cmd(const ezb_zcl_cmd_hdr_t *header, const uint8_t *payload,
                                                 uint16_t payload_length)
{
    (void)payload;
    (void)payload_length;
    if (header == NULL || header->cmd_id != SHELF_CMD_BUTTON_PRESSED_ID) {
        return EZB_ZCL_STATUS_SUCCESS;
    }

    ezb_extaddr_t ext_addr;
    if (ezb_address_extended_by_short(header->src_addr.u.short_addr, &ext_addr) != EZB_ERR_NONE) {
        return EZB_ZCL_STATUS_SUCCESS;
    }

    uint8_t ieee[8];
    ieee_bytes_from_extaddr(&ext_addr, ieee);
    message_handler_emit_button_pressed(ieee);
    return EZB_ZCL_STATUS_SUCCESS;
}

static void zigbee_coordinator_register_event_handlers(void)
{
    ezb_zcl_custom_cluster_handlers_t handlers = {
        .cluster_id     = SHELF_CLUSTER_EVENTS_ID,
        .cluster_role   = EZB_ZCL_CLUSTER_CLIENT,
        .process_cmd_cb = coord_events_process_cmd,
    };
    ESP_ERROR_CHECK(ezb_zcl_custom_cluster_handlers_register(&handlers));
}

static void esp_zigbee_zcl_core_action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    if (callback_id == EZB_ZCL_CORE_REPORT_ATTR_CB_ID) {
        zcl_report_attr_handler(message);
    }
}

static esp_err_t zigbee_coordinator_create_gateway_device(void)
{
    ezb_af_device_desc_t dev_desc = ezb_af_create_device_desc();
    ezb_zha_on_off_switch_config_t switch_cfg = EZB_ZHA_ON_OFF_SWITCH_CONFIG();
    ezb_af_ep_desc_t ep_desc = ezb_zha_create_on_off_switch(SHELF_HA_GATEWAY_EP_ID, &switch_cfg);

    ezb_zcl_cluster_desc_t basic_desc =
        ezb_af_endpoint_get_cluster_desc(ep_desc, EZB_ZCL_CLUSTER_ID_BASIC, EZB_ZCL_CLUSTER_SERVER);
    ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
                                        (void *)SHELF_MANUFACTURER_NAME);
    ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
                                        (void *)SHELF_MODEL_IDENTIFIER);

    ezb_zcl_custom_cluster_config_t events_cfg = {
        .cluster_id = SHELF_CLUSTER_EVENTS_ID,
    };
    ezb_zcl_cluster_desc_t events_client =
        ezb_zcl_custom_create_cluster_desc(&events_cfg, EZB_ZCL_CLUSTER_CLIENT);
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep_desc, events_client));

    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(dev_desc, ep_desc));
    ESP_ERROR_CHECK(ezb_af_device_desc_register(dev_desc));
    ezb_zcl_core_action_handler_register(esp_zigbee_zcl_core_action_handler);
    zigbee_coordinator_register_event_handlers();

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
            if (ezb_bdb_is_factory_new()) {
                ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_FORMATION);
            } else {
                ezb_bdb_open_network(180);
            }
        } else {
            alarm_timer_schedule(esp_zigbee_alarm_bdb_commissioning, EZB_BDB_MODE_INITIALIZATION, 1000);
        }
    } break;
    case EZB_BDB_SIGNAL_FORMATION: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Network formed on channel %d PAN 0x%04hx", ezb_nwk_get_current_channel(),
                     ezb_nwk_get_panid());
            ezb_bdb_open_network(180);
        } else {
            alarm_timer_schedule(esp_zigbee_alarm_bdb_commissioning, EZB_BDB_MODE_NETWORK_FORMATION, 1000);
        }
    } break;
    case EZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        const ezb_zdo_signal_device_annce_params_t *annce = ezb_app_signal_get_params(app_signal);
        ESP_LOGI(TAG, "Device joined/rejoined short 0x%04hx", annce->short_addr);
        zigbee_coordinator_register_joined_device(annce->short_addr);
    } break;
    case EZB_ZDO_SIGNAL_LEAVE_INDICATION: {
        const ezb_zdo_signal_leave_indication_params_t *leave = ezb_app_signal_get_params(app_signal);
        device_registry_remove_by_short(leave->short_addr);
    } break;
    default:
        break;
    }

    return true;
}

static void zigbee_coordinator_stack_task(void *arg)
{
    (void)arg;

    esp_zigbee_config_t config = {
        .device_config =
            {
                .device_type = EZB_NWK_DEVICE_TYPE_COORDINATOR,
                .install_code_policy = false,
                .zczr_config =
                    {
                        .max_children = SHELF_MAX_CHILDREN_ZC,
                    },
            },
        .platform_config =
            {
                .storage_partition_name = SHELF_ZIGBEE_STORAGE_PARTITION_NAME,
                .radio_config =
                    {
                        .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE,
                    },
            },
    };

    ESP_ERROR_CHECK(esp_zigbee_init(&config));
    ESP_ERROR_CHECK(shelf_zigbee_apply_coordinator_memory());

    ezb_aps_secur_enable_distributed_security(false);
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(SHELF_PRIMARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_bdb_set_secondary_channel_set(SHELF_SECONDARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(esp_zigbee_app_signal_handler));
    ESP_ERROR_CHECK(zigbee_coordinator_create_gateway_device());
    ESP_ERROR_CHECK(esp_zigbee_start(false));

    esp_zigbee_launch_mainloop();
    esp_zigbee_deinit();
    vTaskDelete(NULL);
}

esp_err_t zigbee_coordinator_init(void)
{
    return ESP_OK;
}

void zigbee_coordinator_start_task(void)
{
    xTaskCreate(zigbee_coordinator_stack_task, "Zigbee_main", 6144, NULL, 5, NULL);
}
