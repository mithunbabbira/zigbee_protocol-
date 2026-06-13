/*
 * Shelf Zigbee Coordinator — network gateway with UART protocol bridge.
 */
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "alarm_timer.h"

#include "esp_zigbee.h"
#include "ezbee/nwk.h"
#include "ezbee/zha.h"

#include "coordinator.h"
#include "shelf_zigbee.h"

static const char *TAG = "COORDINATOR";

#define COORDINATOR_MAX_KNOWN_SHELVES 32U
#define COORDINATOR_RESCAN_INTERVAL_MS 5000U
#define COORDINATOR_RESCAN_REPEATS 24U
#define COORDINATOR_NEIGHBOR_MAX_AGE 5U
#define COORDINATOR_LIVENESS_SCAN_MS 2500U

static uint16_t s_known_shelves[COORDINATOR_MAX_KNOWN_SHELVES];
static size_t s_known_shelf_count;
static uint8_t s_rescan_remaining;
static bool s_liveness_scan_active;
static uint16_t s_liveness_scan_addrs[COORDINATOR_MAX_KNOWN_SHELVES];
static bool s_liveness_scan_responded[COORDINATOR_MAX_KNOWN_SHELVES];
static size_t s_liveness_scan_count;

static bool esp_zigbee_app_signal_handler(const ezb_app_signal_t *app_signal);
static void coordinator_rescan_alarm(alarm_timer_arg_t arg);
static void coordinator_rescan_neighbor_shelves(void);
static void coordinator_run_liveness_scan(void);
static void coordinator_finalize_liveness_scan(alarm_timer_arg_t arg);
static ezb_err_t coordinator_find_and_provision_shelf(uint16_t short_addr);
static void coordinator_register_shelf(uint16_t short_addr);

static void coordinator_send_unicast_on_off(uint16_t dst_short_addr, uint8_t state)
{
    ezb_zcl_on_off_cmd_t cmd_req = {
        .cmd_ctrl =
            {
                .dst_addr.addr_mode = EZB_ADDR_MODE_SHORT,
                .dst_addr.u.short_addr = dst_short_addr,
                .dst_ep             = SHELF_HA_LIGHT_EP_ID,
                .src_ep             = SHELF_HA_GATEWAY_EP_ID,
            },
    };

    esp_zigbee_lock_acquire(portMAX_DELAY);
    if (state) {
        ezb_zcl_on_off_on_cmd_req(&cmd_req);
    } else {
        ezb_zcl_on_off_off_cmd_req(&cmd_req);
    }
    esp_zigbee_lock_release();

    ESP_LOGI(TAG, "Sent unicast %s to 0x%04hx", state ? "ON" : "OFF", dst_short_addr);
}

static void coordinator_send_groupcast_on_off(uint16_t group_id, uint8_t state)
{
    ezb_zcl_on_off_cmd_t cmd_req = {
        .cmd_ctrl =
            {
                .dst_addr = EZB_ADDRESS_GROUP(group_id, 0xFFFD),
                .src_ep   = SHELF_HA_GATEWAY_EP_ID,
            },
    };

    esp_zigbee_lock_acquire(portMAX_DELAY);
    if (state) {
        ezb_zcl_on_off_on_cmd_req(&cmd_req);
    } else {
        ezb_zcl_on_off_off_cmd_req(&cmd_req);
    }
    esp_zigbee_lock_release();

    ESP_LOGI(TAG, "Sent groupcast %s to group 0x%04x", state ? "ON" : "OFF", group_id);
}

static void coordinator_handle_gateway_command(const shelf_gateway_cmd_t *cmd)
{
    if (cmd == NULL || cmd->type == SHELF_GATEWAY_CMD_NONE) {
        return;
    }

    if (!ezb_bdb_dev_joined()) {
        ESP_LOGW(TAG, "Ignoring gateway command, network not ready");
        return;
    }

    switch (cmd->type) {
    case SHELF_GATEWAY_CMD_NODE_SEND:
        coordinator_send_unicast_on_off(cmd->addr, cmd->state);
        break;
    case SHELF_GATEWAY_CMD_GROUP_SEND:
        coordinator_send_groupcast_on_off(cmd->addr, cmd->state);
        break;
    case SHELF_GATEWAY_CMD_NODE_SCAN:
        esp_zigbee_lock_acquire(portMAX_DELAY);
        coordinator_run_liveness_scan();
        esp_zigbee_lock_release();
        break;
    default:
        break;
    }
}

static void uart_gateway_task(void *arg)
{
    (void)arg;

    usb_serial_jtag_driver_config_t usb_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_cfg));

    char line[SHELF_UART_LINE_MAX];
    size_t pos = 0;

    ESP_LOGI(TAG, "UART gateway ready. Commands: NODE_SEND:0xADDR:STATE:0|1, GROUP_SEND:0xGROUP:STATE:0|1");

    while (true) {
        uint8_t byte;
        int read = usb_serial_jtag_read_bytes(&byte, 1, pdMS_TO_TICKS(100));
        if (read <= 0) {
            continue;
        }

        if (byte == '\n' || byte == '\r') {
            if (pos > 0) {
                line[pos] = '\0';
                shelf_gateway_cmd_t cmd;
                if (shelf_gateway_parse_line(line, &cmd)) {
                    coordinator_handle_gateway_command(&cmd);
                } else {
                    ESP_LOGW(TAG, "Unknown gateway command: %s", line);
                }
                pos = 0;
            }
            continue;
        }

        if (pos < sizeof(line) - 1U) {
            line[pos++] = (char)byte;
        }
    }
}

static void coordinator_emit_gateway_line(const char *line)
{
    printf("%s", line);
    fflush(stdout);
}

static void coordinator_emit_node_recv(uint16_t short_addr, uint8_t state)
{
    char out[SHELF_UART_LINE_MAX];
    shelf_gateway_format_node_recv(short_addr, state, out, sizeof(out));
    coordinator_emit_gateway_line(out);
    ESP_LOGI(TAG, "Upstream %s", out);
}

static bool coordinator_is_known_shelf(uint16_t short_addr)
{
    for (size_t i = 0; i < s_known_shelf_count; i++) {
        if (s_known_shelves[i] == short_addr) {
            return true;
        }
    }
    return false;
}

static void coordinator_mark_known_shelf(uint16_t short_addr)
{
    if (coordinator_is_known_shelf(short_addr) || s_known_shelf_count >= COORDINATOR_MAX_KNOWN_SHELVES) {
        return;
    }
    s_known_shelves[s_known_shelf_count++] = short_addr;
}

static void coordinator_broadcast_node_join(uint16_t short_addr)
{
    char out[SHELF_UART_LINE_MAX];
    shelf_gateway_format_node_join(short_addr, out, sizeof(out));
    coordinator_emit_gateway_line(out);
    ESP_LOGI(TAG, "Shelf joined %s", out);
}

static void coordinator_register_shelf(uint16_t short_addr)
{
    coordinator_broadcast_node_join(short_addr);
    if (coordinator_is_known_shelf(short_addr)) {
        return;
    }
    coordinator_mark_known_shelf(short_addr);
    coordinator_find_and_provision_shelf(short_addr);
}

static void coordinator_emit_node_leave(uint16_t short_addr)
{
    char out[SHELF_UART_LINE_MAX];
    shelf_gateway_format_node_leave(short_addr, out, sizeof(out));
    coordinator_emit_gateway_line(out);
    ESP_LOGI(TAG, "Shelf left %s", out);
}

static void zdo_bind_shelf_device_result(const ezb_zdp_bind_req_result_t *result, void *user_ctx)
{
    uint16_t short_addr = (uint16_t)(uintptr_t)user_ctx;

    if (result == NULL) {
        return;
    }

    if (result->error == EZB_ERR_NONE && result->rsp && result->rsp->status == EZB_ZDP_STATUS_SUCCESS) {
        if (short_addr != 0) {
            coordinator_broadcast_node_join(short_addr);
        }
        ESP_LOGI(TAG, "Bound shelf device successfully");
    } else if (result->error != EZB_ERR_NONE) {
        ESP_LOGE(TAG, "Bind failed with error 0x%04x", result->error);
    } else if (result->rsp) {
        ESP_LOGE(TAG, "Bind failed with ZDP status 0x%02x", result->rsp->status);
    }
}

static ezb_err_t coordinator_bind_shelf_device(uint16_t dst_short_addr, uint8_t dst_ep)
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
                        "Failed to resolve extended address for 0x%04hx", dst_short_addr);

    ezb_err_t ret = ezb_zdo_bind_req(&bind_req);
    if (ret == EZB_ERR_NONE) {
        ESP_LOGI(TAG, "Binding shelf 0x%04hx ep %u", dst_short_addr, dst_ep);
    } else {
        ESP_LOGE(TAG, "Bind request failed for 0x%04hx (0x%04x)", dst_short_addr, ret);
    }
    return ret;
}

static void coordinator_add_shelf_to_group(uint16_t dst_short_addr, uint8_t dst_ep, uint16_t group_id)
{
    ezb_zcl_groups_add_group_cmd_t add_group_cmd = {
        .cmd_ctrl =
            {
                .dst_addr.addr_mode = EZB_ADDR_MODE_SHORT,
                .dst_addr.u.short_addr = dst_short_addr,
                .dst_ep             = dst_ep,
                .src_ep             = SHELF_HA_GATEWAY_EP_ID,
            },
        .payload =
            {
                .group_id   = group_id,
                .group_name = NULL,
            },
    };

    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_err_t ret = ezb_zcl_groups_add_group_cmd_req(&add_group_cmd);
    esp_zigbee_lock_release();

    if (ret == EZB_ERR_NONE) {
        ESP_LOGI(TAG, "Added shelf 0x%04hx to group 0x%04x", dst_short_addr, group_id);
    } else {
        ESP_LOGE(TAG, "Failed to add shelf 0x%04hx to group 0x%04x (0x%04x)", dst_short_addr, group_id, ret);
    }
}

static void zdo_find_shelf_device_result(const ezb_zdo_match_desc_req_result_t *result, void *user_ctx)
{
    (void)user_ctx;
    if (result == NULL || result->error != EZB_ERR_NONE) {
        if (result) {
            ESP_LOGE(TAG, "Match descriptor failed (0x%04x)", result->error);
        }
        return;
    }

    if (result->rsp && result->rsp->status == EZB_ZDP_STATUS_SUCCESS && result->rsp->match_length > 0 &&
        result->rsp->match_list) {
        for (size_t i = 0; i < result->rsp->match_length; i++) {
            uint8_t ep = result->rsp->match_list[i];
            coordinator_bind_shelf_device(result->rsp->nwk_addr_of_interest, ep);
            coordinator_add_shelf_to_group(result->rsp->nwk_addr_of_interest, ep, SHELF_DEFAULT_GROUP_ID);
        }
    }
}

static ezb_err_t coordinator_find_and_provision_shelf(uint16_t short_addr)
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

    ezb_err_t ret = ezb_zdo_match_desc_req(&req);
    if (ret == EZB_ERR_NONE) {
        ESP_LOGI(TAG, "Provisioning shelf 0x%04hx", short_addr);
    }
    return ret;
}

static void esp_zigbee_alarm_bdb_commissioning(alarm_timer_arg_t arg)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    (void)ezb_bdb_start_top_level_commissioning(arg);
    esp_zigbee_lock_release();
}

static bool coordinator_addr_in_list(uint16_t addr, const uint16_t *list, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (list[i] == addr) {
            return true;
        }
    }
    return false;
}

static int coordinator_liveness_index(uint16_t short_addr)
{
    for (size_t i = 0; i < s_liveness_scan_count; i++) {
        if (s_liveness_scan_addrs[i] == short_addr) {
            return (int)i;
        }
    }
    return -1;
}

static void coordinator_mark_liveness_response(uint16_t short_addr)
{
    if (!s_liveness_scan_active) {
        return;
    }
    int idx = coordinator_liveness_index(short_addr);
    if (idx >= 0) {
        s_liveness_scan_responded[idx] = true;
    }
}

static void coordinator_ping_shelf_on_off(uint16_t short_addr)
{
    static uint16_t on_off_attr_id = EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID;
    ezb_zcl_read_attr_cmd_t cmd_req = {
        .cmd_ctrl =
            {
                .dst_addr.addr_mode = EZB_ADDR_MODE_SHORT,
                .dst_addr.u.short_addr = short_addr,
                .dst_ep             = SHELF_HA_LIGHT_EP_ID,
                .src_ep             = SHELF_HA_GATEWAY_EP_ID,
                .cluster_id         = EZB_ZCL_CLUSTER_ID_ON_OFF,
            },
        .payload =
            {
                .attr_number = 1,
                .attr_field  = &on_off_attr_id,
            },
    };

    ezb_err_t ret = ezb_zcl_read_attr_cmd_req(&cmd_req);
    if (ret != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "Ping shelf 0x%04hx failed (0x%04x)", short_addr, ret);
    }
}

static void coordinator_finalize_liveness_scan(alarm_timer_arg_t arg)
{
    (void)arg;
    esp_zigbee_lock_acquire(portMAX_DELAY);
    if (!s_liveness_scan_active) {
        esp_zigbee_lock_release();
        return;
    }
    s_liveness_scan_active = false;
    for (size_t i = 0; i < s_liveness_scan_count; i++) {
        if (s_liveness_scan_responded[i]) {
            coordinator_broadcast_node_join(s_liveness_scan_addrs[i]);
        } else {
            ESP_LOGI(TAG, "Shelf 0x%04hx offline (no ping response)", s_liveness_scan_addrs[i]);
            coordinator_emit_node_leave(s_liveness_scan_addrs[i]);
        }
    }
    esp_zigbee_lock_release();
}

static void coordinator_run_liveness_scan(void)
{
    if (s_liveness_scan_active) {
        return;
    }

    s_liveness_scan_count = 0;
    memset(s_liveness_scan_responded, 0, sizeof(s_liveness_scan_responded));
    for (size_t i = 0; i < s_known_shelf_count && s_liveness_scan_count < COORDINATOR_MAX_KNOWN_SHELVES; i++) {
        s_liveness_scan_addrs[s_liveness_scan_count++] = s_known_shelves[i];
    }

    if (s_liveness_scan_count == 0) {
        coordinator_rescan_neighbor_shelves();
        return;
    }

    s_liveness_scan_active = true;
    ESP_LOGI(TAG, "Liveness scan for %u shelf(s)", (unsigned)s_liveness_scan_count);
    for (size_t i = 0; i < s_liveness_scan_count; i++) {
        coordinator_ping_shelf_on_off(s_liveness_scan_addrs[i]);
    }
    alarm_timer_schedule(coordinator_finalize_liveness_scan, 0, COORDINATOR_LIVENESS_SCAN_MS);
}

static void coordinator_rescan_neighbor_shelves(void)
{
    ezb_nwk_info_iterator_t iterator = NULL;
    ezb_nwk_neighbor_info_t neighbor;
    uint16_t self_addr = ezb_nwk_get_short_address();
    uint16_t found[COORDINATOR_MAX_KNOWN_SHELVES];
    size_t found_count = 0;

    while (ezb_nwk_get_next_neighbor(&iterator, &neighbor) == EZB_ERR_NONE) {
        if (neighbor.device_type != EZB_NWK_DEVICE_TYPE_ROUTER || neighbor.short_addr == self_addr) {
            continue;
        }
        if (neighbor.age > COORDINATOR_NEIGHBOR_MAX_AGE) {
            ESP_LOGI(TAG, "Skipping stale neighbor 0x%04hx (age %u)", neighbor.short_addr, neighbor.age);
            continue;
        }
        ESP_LOGI(TAG, "Found shelf router 0x%04hx in neighbor table", neighbor.short_addr);
        if (!coordinator_addr_in_list(neighbor.short_addr, found, found_count) &&
            found_count < COORDINATOR_MAX_KNOWN_SHELVES) {
            found[found_count++] = neighbor.short_addr;
        }
        coordinator_register_shelf(neighbor.short_addr);
    }

    for (size_t i = 0; i < s_known_shelf_count; i++) {
        uint16_t addr = s_known_shelves[i];
        if (!coordinator_addr_in_list(addr, found, found_count)) {
            coordinator_emit_node_leave(addr);
        }
    }
}

static void coordinator_start_periodic_rescan(void)
{
    s_rescan_remaining = COORDINATOR_RESCAN_REPEATS;
    alarm_timer_schedule(coordinator_rescan_alarm, 0, COORDINATOR_RESCAN_INTERVAL_MS);
}

static void coordinator_rescan_alarm(alarm_timer_arg_t arg)
{
    (void)arg;
    esp_zigbee_lock_acquire(portMAX_DELAY);
    coordinator_rescan_neighbor_shelves();
    esp_zigbee_lock_release();

    if (s_rescan_remaining > 0) {
        s_rescan_remaining--;
        alarm_timer_schedule(coordinator_rescan_alarm, 0, COORDINATOR_RESCAN_INTERVAL_MS);
    }
}

static void zcl_core_cmd_read_attr_rsp_handler(ezb_zcl_cmd_read_attr_rsp_message_t *message)
{
    ESP_RETURN_ON_FALSE(message, , TAG, "Empty read attribute response");
    ESP_RETURN_ON_FALSE(message->in.header, , TAG, "Empty read attribute header");

    uint16_t src_short = message->in.header->src_addr.u.short_addr;
    coordinator_mark_liveness_response(src_short);

    if (message->info.cluster_id != EZB_ZCL_CLUSTER_ID_ON_OFF) {
        return;
    }

    for (ezb_zcl_read_attr_rsp_variable_t *var = message->in.variables; var != NULL; var = var->next) {
        if (var->attr_id == EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID && var->status == EZB_ZCL_STATUS_SUCCESS &&
            var->attr_value != NULL) {
            uint8_t state = (*(uint8_t *)var->attr_value) ? 1U : 0U;
            coordinator_emit_node_recv(src_short, state);
        }
    }
}

static void zcl_core_cmd_report_attr_handler(ezb_zcl_cmd_report_attr_message_t *message)
{
    ESP_RETURN_ON_FALSE(message, , TAG, "Empty report attribute message");
    if (message->info.cluster_id != EZB_ZCL_CLUSTER_ID_ON_OFF || message->in.header == NULL) {
        return;
    }

    uint16_t src_short = message->in.header->src_addr.u.short_addr;
    for (ezb_zcl_report_attr_variable_t *var = message->in.variables; var != NULL; var = var->next) {
        if (var->attr_id == EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID && var->attr_type == EZB_ZCL_ATTR_TYPE_BOOL) {
            uint8_t state = (*(uint8_t *)var->attr_value) ? 1U : 0U;
            coordinator_emit_node_recv(src_short, state);
        }
    }
}

static void esp_zigbee_zcl_core_action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    switch (callback_id) {
    case EZB_ZCL_CORE_REPORT_ATTR_CB_ID:
        zcl_core_cmd_report_attr_handler(message);
        break;
    case EZB_ZCL_CORE_READ_ATTR_RSP_CB_ID:
        zcl_core_cmd_read_attr_rsp_handler(message);
        break;
    case EZB_ZCL_CORE_GROUPS_ADD_GROUP_RSP_CB_ID: {
        ezb_zcl_groups_add_group_rsp_message_t *rsp = message;
        ESP_LOGI(TAG, "Add group response: group 0x%04x status 0x%02x", rsp->in.group_id, rsp->in.status);
    } break;
    case EZB_ZCL_CORE_DEFAULT_RSP_CB_ID: {
        ezb_zcl_cmd_default_rsp_message_t *default_rsp = message;
        ESP_LOGD(TAG, "ZCL default response status 0x%02x", default_rsp->in.status_code);
    } break;
    default:
        ESP_LOGD(TAG, "Unhandled ZCL action 0x%08lx", callback_id);
        break;
    }
}

static esp_err_t coordinator_create_gateway_device(void)
{
    ezb_af_device_desc_t dev_desc = ezb_af_create_device_desc();
    ezb_zha_on_off_switch_config_t switch_cfg = EZB_ZHA_ON_OFF_SWITCH_CONFIG();
    ezb_af_ep_desc_t ep_desc = ezb_zha_create_on_off_switch(SHELF_HA_GATEWAY_EP_ID, &switch_cfg);
    ezb_zcl_cluster_desc_t basic_desc =
        ezb_af_endpoint_get_cluster_desc(ep_desc, EZB_ZCL_CLUSTER_ID_BASIC, EZB_ZCL_CLUSTER_SERVER);
    ezb_zcl_cluster_desc_t groups_desc = ezb_zcl_groups_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT);

    ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
                                        (void *)SHELF_MANUFACTURER_NAME);
    ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
                                        (void *)SHELF_MODEL_IDENTIFIER);
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep_desc, groups_desc));
    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(dev_desc, ep_desc));
    ESP_ERROR_CHECK(ezb_af_device_desc_register(dev_desc));
    ezb_zcl_core_action_handler_register(esp_zigbee_zcl_core_action_handler);

    return ESP_OK;
}

static esp_err_t coordinator_setup_commissioning(void)
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
            ESP_LOGI(TAG, "Started in%s factory-reset mode", ezb_bdb_is_factory_new() ? "" : " non");
            if (ezb_bdb_is_factory_new()) {
                ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_FORMATION);
            } else {
                ezb_bdb_open_network(180);
                ESP_LOGI(TAG, "Network reopened for 180 seconds");
                coordinator_start_periodic_rescan();
            }
        } else {
            ESP_LOGW(TAG, "%s failed (0x%02x), retrying", ezb_app_signal_to_string(signal_type), status);
            alarm_timer_schedule(esp_zigbee_alarm_bdb_commissioning, EZB_BDB_MODE_INITIALIZATION, 1000);
        }
    } break;
    case EZB_BDB_SIGNAL_FORMATION: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ezb_extpanid_t extended_pan_id;
            ezb_nwk_get_extended_panid(&extended_pan_id);
            ESP_LOGI(TAG, "Network formed: PAN 0x%04hx, channel %d, short addr 0x%04hx",
                     ezb_nwk_get_panid(), ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
            ezb_bdb_open_network(180);
            ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
            coordinator_start_periodic_rescan();
        } else {
            ESP_LOGW(TAG, "Formation failed (0x%02x), retrying", status);
            alarm_timer_schedule(esp_zigbee_alarm_bdb_commissioning, EZB_BDB_MODE_NETWORK_FORMATION, 1000);
        }
    } break;
    case EZB_BDB_SIGNAL_STEERING: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Coordinator steering complete");
        } else {
            ESP_LOGW(TAG, "Steering failed (0x%02x)", status);
        }
    } break;
    case EZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        const ezb_zdo_signal_device_annce_params_t *dev_annce = ezb_app_signal_get_params(app_signal);
        ESP_LOGI(TAG, "Shelf joined or rejoined: short addr 0x%04hx", dev_annce->short_addr);
        coordinator_register_shelf(dev_annce->short_addr);
    } break;
    case EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS: {
        uint8_t duration = *(uint8_t *)ezb_app_signal_get_params(app_signal);
        ESP_LOGI(TAG, "Permit join %u seconds on PAN 0x%04hx", duration, ezb_nwk_get_panid());
    } break;
    case EZB_ZDO_SIGNAL_LEAVE_INDICATION: {
        const ezb_zdo_signal_leave_indication_params_t *leave_ind =
            ezb_app_signal_get_params(app_signal);
        ESP_LOGI(TAG, "Shelf left network: short addr 0x%04hx", leave_ind->short_addr);
        coordinator_emit_node_leave(leave_ind->short_addr);
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
    ESP_ERROR_CHECK(shelf_zigbee_apply_coordinator_memory());
    ESP_ERROR_CHECK(coordinator_setup_commissioning());
    ESP_ERROR_CHECK(coordinator_create_gateway_device());
    ESP_ERROR_CHECK(esp_zigbee_start(false));

    xTaskCreate(uart_gateway_task, "uart_gateway", 4096, NULL, 4, NULL);

    esp_zigbee_launch_mainloop();
    esp_zigbee_deinit();
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(nvs_flash_init_partition(SHELF_ZIGBEE_STORAGE_PARTITION_NAME));
    ESP_LOGI(TAG, "Coordinator starting (Zigbee Coordinator + UART gateway)");
    xTaskCreate(esp_zigbee_stack_main_task, "Zigbee_main", 6144, NULL, 5, NULL);
}
