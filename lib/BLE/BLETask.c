/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "BLETask.h"




#include "nvs_flash.h"

#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_generic_model_api.h"
#include "esp_ble_mesh_local_data_operation_api.h"
#include <stdio.h>
#include "esp_log.h"
#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_generic_model_api.h"

#include <stdarg.h>

#include "board.h"
#include "ble_mesh_init.h"
#include "dev_identity.h"
#include "device_mode.h"
#include "cmd_proto.h"
#include "mesh_attr_model.h"
#include "spi_cmd.h"
#include "uart_cmd.h"

#define TAG "EXAMPLE"

#define CID_ESP 0x02E5

/* Provisioner (gateway) parameters. PROV_OWN_ADDR is the address this device
 * uses when acting as Provisioner; provisioned nodes are handed addresses from
 * PROV_START_ADDR upwards. A gateway should be the provisioner of its own
 * network -- if it is also provisioned by an external app it will end up with
 * two conflicting unicast addresses. */
#define PROV_OWN_ADDR   0x0001
#define PROV_START_ADDR 0x0005
#define APP_KEY_IDX     0x0000
#define APP_KEY_OCTET   0x12


TickType_t ticks = 0;


extern struct _led_state led_state[3];

static uint8_t dev_uuid[16] = { 0xdd, 0xdd };

static struct esp_ble_mesh_key {
    uint16_t net_idx;
    uint16_t app_idx;
    uint8_t  app_key[16];
} prov_key;

static esp_ble_mesh_cfg_srv_t config_server = {
    .relay = ESP_BLE_MESH_RELAY_DISABLED,
    .beacon = ESP_BLE_MESH_BEACON_ENABLED,
#if defined(CONFIG_BLE_MESH_FRIEND)
    .friend_state = ESP_BLE_MESH_FRIEND_ENABLED,
#else
    .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
#endif
#if defined(CONFIG_BLE_MESH_GATT_PROXY_SERVER)
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_ENABLED,
#else
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_NOT_SUPPORTED,
#endif
    .default_ttl = 7,
    /* 3 transmissions with 20ms interval */
    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
};

ESP_BLE_MESH_MODEL_PUB_DEFINE(onoff_pub_0, 2 + 3, ROLE_NODE);
static esp_ble_mesh_gen_onoff_srv_t onoff_server_0 = {
    .rsp_ctrl.get_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP,
    .rsp_ctrl.set_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP,
};

ESP_BLE_MESH_MODEL_PUB_DEFINE(onoff_pub_1, 2 + 3, ROLE_NODE);
static esp_ble_mesh_gen_onoff_srv_t onoff_server_1 = {
    .rsp_ctrl.get_auto_rsp = ESP_BLE_MESH_SERVER_RSP_BY_APP,
    .rsp_ctrl.set_auto_rsp = ESP_BLE_MESH_SERVER_RSP_BY_APP,
};

ESP_BLE_MESH_MODEL_PUB_DEFINE(onoff_pub_2, 2 + 3, ROLE_NODE);
static esp_ble_mesh_gen_onoff_srv_t onoff_server_2 = {
    .rsp_ctrl.get_auto_rsp = ESP_BLE_MESH_SERVER_AUTO_RSP,
    .rsp_ctrl.set_auto_rsp = ESP_BLE_MESH_SERVER_RSP_BY_APP,
};

static esp_ble_mesh_client_t onoff_client;
ESP_BLE_MESH_MODEL_PUB_DEFINE(onoff_cli_pub, 2 + 1, ROLE_NODE);

/* Only exercised in gateway mode, but the composition is fixed at
 * esp_ble_mesh_init() time so the model is always present. */
static esp_ble_mesh_client_t config_client;

static esp_ble_mesh_model_t root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&config_server),
    ESP_BLE_MESH_MODEL_CFG_CLI(&config_client),
    ESP_BLE_MESH_MODEL_GEN_ONOFF_SRV(&onoff_pub_0, &onoff_server_0),
    ESP_BLE_MESH_MODEL_GEN_ONOFF_CLI(&onoff_cli_pub, &onoff_client),
};

static esp_ble_mesh_model_t extend_model_0[] = {
    ESP_BLE_MESH_MODEL_GEN_ONOFF_SRV(&onoff_pub_1, &onoff_server_1),
};

static esp_ble_mesh_model_t extend_model_1[] = {
    ESP_BLE_MESH_MODEL_GEN_ONOFF_SRV(&onoff_pub_2, &onoff_server_2),
};

static esp_ble_mesh_elem_t elements[] = {
    /* The attribute vendor models ride in element 0's vendor slot, which was
     * previously unused. Present in both personalities: a gateway asks with the
     * client half, a node answers with the server half. */
    ESP_BLE_MESH_ELEMENT(0, root_models, mesh_attr_vnd_models),
    ESP_BLE_MESH_ELEMENT(0, extend_model_0, ESP_BLE_MESH_MODEL_NONE),
    ESP_BLE_MESH_ELEMENT(0, extend_model_1, ESP_BLE_MESH_MODEL_NONE),
};

static esp_ble_mesh_comp_t composition = {
    .cid = CID_ESP,
    .elements = elements,
    .element_count = ARRAY_SIZE(elements),
};

/* Disable OOB security for SILabs Android app.
 * Carries both role's parameters: the node fields are used when this device is
 * provisioned by someone else, the prov_* fields when it acts as gateway. */
static esp_ble_mesh_prov_t provision = {
    .uuid = dev_uuid,
#if 0
    .output_size = 4,
    .output_actions = ESP_BLE_MESH_DISPLAY_NUMBER,
    .input_actions = ESP_BLE_MESH_PUSH,
    .input_size = 4,
#else
    .output_size = 0,
    .output_actions = 0,
#endif
    .prov_uuid           = dev_uuid,
    .prov_unicast_addr   = PROV_OWN_ADDR,
    .prov_start_address  = PROV_START_ADDR,
    .prov_attention      = 0x00,
    .prov_algorithm      = 0x00,
    .prov_pub_key_oob    = 0x00,
    .prov_static_oob_val = NULL,
    .prov_static_oob_len = 0x00,
    .flags               = 0x00,
    .iv_index            = 0x00,
};

static void prov_complete(uint16_t net_idx, uint16_t addr, uint8_t flags, uint32_t iv_index)
{
    ESP_LOGI(TAG, "net_idx: 0x%04x, addr: 0x%04x", net_idx, addr);
    ESP_LOGI(TAG, "flags: 0x%02x, iv_index: 0x%08"PRIx32, flags, iv_index);
    board_led_operation(LED_G, LED_OFF);

    /* Now that an element address exists, the stored group can finally be
     * subscribed. This is what restores group membership across a reboot. */
    mesh_attr_apply_group();
}

static void example_change_led_state(esp_ble_mesh_model_t *model,
                                     esp_ble_mesh_msg_ctx_t *ctx, uint8_t onoff)
{
    uint16_t primary_addr = esp_ble_mesh_get_primary_element_address();
    uint8_t elem_count = esp_ble_mesh_get_element_count();
    struct _led_state *led = NULL;
    uint8_t i;

    if (ESP_BLE_MESH_ADDR_IS_UNICAST(ctx->recv_dst)) {
        for (i = 0; i < elem_count; i++) {
            if (ctx->recv_dst == (primary_addr + i)) {
                led = &led_state[i];
                board_led_operation(led->pin, onoff);
            }
        }
    } else if (ESP_BLE_MESH_ADDR_IS_GROUP(ctx->recv_dst)) {
        if (esp_ble_mesh_is_model_subscribed_to_group(model, ctx->recv_dst)) {
            led = &led_state[model->element->element_addr - primary_addr];
            board_led_operation(led->pin, onoff);
        }
    } else if (ctx->recv_dst == 0xFFFF) {
        led = &led_state[model->element->element_addr - primary_addr];
        board_led_operation(led->pin, onoff);
    }
}

static void example_handle_gen_onoff_msg(esp_ble_mesh_model_t *model,
                                         esp_ble_mesh_msg_ctx_t *ctx,
                                         esp_ble_mesh_server_recv_gen_onoff_set_t *set)
{
    esp_ble_mesh_gen_onoff_srv_t *srv = model->user_data;

    switch (ctx->recv_op) {
    case ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET:
        esp_ble_mesh_server_model_send_msg(model, ctx,
            ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_STATUS, sizeof(srv->state.onoff), &srv->state.onoff);
        break;
    case ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET:
    case ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK:
        if (set->op_en == false) {
            srv->state.onoff = set->onoff;
        } else {
            /* TODO: Delay and state transition */
            srv->state.onoff = set->onoff;
        }
        if (ctx->recv_op == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET) {
            esp_ble_mesh_server_model_send_msg(model, ctx,
                ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_STATUS, sizeof(srv->state.onoff), &srv->state.onoff);
        }
        /* The publish role picks which app-key list the stack searches; a
         * gateway's key lives in the provisioner list, so publishing as
         * ROLE_NODE there fails with "Invalid AppKeyIndex". */
        esp_ble_mesh_model_publish(model, ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_STATUS,
            sizeof(srv->state.onoff), &srv->state.onoff,
            device_mode_get() == DEVICE_MODE_GATEWAY ? ROLE_PROVISIONER : ROLE_NODE);
        example_change_led_state(model, ctx, srv->state.onoff);
        break;
    default:
        break;
    }
}

/**
 * @brief Forward a mesh event to the host over every command transport.
 *
 * Gateway role only -- a plain node has no host attached and would just be
 * talking to itself. Output goes to the same links the mode commands arrive on,
 * so a host sees traffic in the format it already speaks.
 */
static void gateway_bridge_emitf(const char *fmt, ...)
{
    if (device_mode_get() != DEVICE_MODE_GATEWAY) {
        return;
    }

    char line[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    ESP_LOGI(TAG, "bridge -> %s", line);
    uart_cmd_emit(line);
    spi_cmd_emit(line);
}

/* Defined below with the rest of the Config Client flow; needed here because
 * provisioning completion is what kicks that flow off. */
static void gateway_configure_node(uint16_t unicast);

/* Gateway role: an unprovisioned device matching our UUID filter showed up,
 * queue it for provisioning straight away. */
static void gateway_recv_unprov_adv_pkt(uint8_t uuid[16], uint8_t addr[BD_ADDR_LEN],
                                        esp_ble_mesh_addr_type_t addr_type, uint16_t oob_info,
                                        uint8_t adv_type, esp_ble_mesh_prov_bearer_t bearer)
{
    esp_ble_mesh_unprov_dev_add_t add_dev = {0};
    esp_err_t err;

    ESP_LOGI(TAG, "unprovisioned device found, addr type %d, adv type %d, oob 0x%04x, bearer %s",
        addr_type, adv_type, oob_info, (bearer & ESP_BLE_MESH_PROV_ADV) ? "PB-ADV" : "PB-GATT");
    ESP_LOG_BUFFER_HEX("dev addr", addr, BD_ADDR_LEN);
    ESP_LOG_BUFFER_HEX("dev uuid", uuid, 16);

    memcpy(add_dev.addr, addr, BD_ADDR_LEN);
    add_dev.addr_type = (uint8_t)addr_type;
    memcpy(add_dev.uuid, uuid, 16);
    add_dev.oob_info = oob_info;
    add_dev.bearer = (uint8_t)bearer;

    err = esp_ble_mesh_provisioner_add_unprov_dev(&add_dev,
            ADD_DEV_RM_AFTER_PROV_FLAG | ADD_DEV_START_PROV_NOW_FLAG | ADD_DEV_FLUSHABLE_DEV_FLAG);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to queue unprovisioned device (err %d)", err);
    }
}

/* Binds the gateway's app key to every local model that speaks it. Runs when
 * the key is first created and again on reboots that restore it from NVS --
 * binding an already-bound model is harmless. */
static void gateway_bind_local_models(void)
{
    /* Bind to both local clients so the gateway can configure nodes and
     * drive their OnOff state. */
    esp_err_t err = esp_ble_mesh_provisioner_bind_app_key_to_local_model(
        PROV_OWN_ADDR, prov_key.app_idx,
        ESP_BLE_MESH_MODEL_ID_CONFIG_CLI, ESP_BLE_MESH_CID_NVAL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to bind appkey to Config Client (err %d)", err);
    }
    err = esp_ble_mesh_provisioner_bind_app_key_to_local_model(
        PROV_OWN_ADDR, prov_key.app_idx,
        ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_CLI, ESP_BLE_MESH_CID_NVAL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to bind appkey to OnOff Client (err %d)", err);
    }
    /* Vendor models take the real company id rather than CID_NVAL.
     * Without this the gateway cannot send a query at all. */
    err = esp_ble_mesh_provisioner_bind_app_key_to_local_model(
        PROV_OWN_ADDR, prov_key.app_idx,
        MESH_ATTR_MODEL_ID_CLIENT, CID_ESP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to bind appkey to attribute Client (err %d)", err);
    }
    /* The server half too, so a gateway can be queried like any node. */
    err = esp_ble_mesh_provisioner_bind_app_key_to_local_model(
        PROV_OWN_ADDR, prov_key.app_idx,
        MESH_ATTR_MODEL_ID_SERVER, CID_ESP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to bind appkey to attribute Server (err %d)", err);
    }
}

static void example_ble_mesh_provisioning_cb(esp_ble_mesh_prov_cb_event_t event,
                                             esp_ble_mesh_prov_cb_param_t *param)
{
    switch (event) {
    /* ---- Provisioner (gateway) events ---- */
    case ESP_BLE_MESH_PROVISIONER_PROV_ENABLE_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_PROV_ENABLE_COMP_EVT, err_code %d",
            param->provisioner_prov_enable_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_DISABLE_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_PROV_DISABLE_COMP_EVT, err_code %d",
            param->provisioner_prov_disable_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_RECV_UNPROV_ADV_PKT_EVT:
        gateway_recv_unprov_adv_pkt(param->provisioner_recv_unprov_adv_pkt.dev_uuid,
                                    param->provisioner_recv_unprov_adv_pkt.addr,
                                    param->provisioner_recv_unprov_adv_pkt.addr_type,
                                    param->provisioner_recv_unprov_adv_pkt.oob_info,
                                    param->provisioner_recv_unprov_adv_pkt.adv_type,
                                    param->provisioner_recv_unprov_adv_pkt.bearer);
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_LINK_OPEN_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_PROV_LINK_OPEN_EVT, bearer %s",
            param->provisioner_prov_link_open.bearer == ESP_BLE_MESH_PROV_ADV ? "PB-ADV" : "PB-GATT");
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_LINK_CLOSE_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_PROV_LINK_CLOSE_EVT, bearer %s, reason 0x%02x",
            param->provisioner_prov_link_close.bearer == ESP_BLE_MESH_PROV_ADV ? "PB-ADV" : "PB-GATT",
            param->provisioner_prov_link_close.reason);
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_COMPLETE_EVT:
        ESP_LOGI(TAG, "node provisioned: unicast 0x%04x, elem_num %d, netkey_idx 0x%04x",
            param->provisioner_prov_complete.unicast_addr,
            param->provisioner_prov_complete.element_num,
            param->provisioner_prov_complete.netkey_idx);
        ESP_LOG_BUFFER_HEX("node uuid", param->provisioner_prov_complete.device_uuid, 16);
        gateway_bridge_emitf("JOINED unicast=0x%04x elems=%d netkey=0x%04x",
            param->provisioner_prov_complete.unicast_addr,
            param->provisioner_prov_complete.element_num,
            param->provisioner_prov_complete.netkey_idx);
        /* Provisioned is not yet queryable -- the node still needs an app key
         * bound to its attribute model. */
        gateway_configure_node(param->provisioner_prov_complete.unicast_addr);
        break;
    case ESP_BLE_MESH_PROVISIONER_ADD_UNPROV_DEV_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_ADD_UNPROV_DEV_COMP_EVT, err_code %d",
            param->provisioner_add_unprov_dev_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_SET_DEV_UUID_MATCH_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_SET_DEV_UUID_MATCH_COMP_EVT, err_code %d",
            param->provisioner_set_dev_uuid_match_comp.err_code);
        break;
    case ESP_BLE_MESH_PROVISIONER_ADD_LOCAL_APP_KEY_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_ADD_LOCAL_APP_KEY_COMP_EVT, err_code %d",
            param->provisioner_add_app_key_comp.err_code);
        if (param->provisioner_add_app_key_comp.err_code == ESP_OK) {
            prov_key.app_idx = param->provisioner_add_app_key_comp.app_idx;
            gateway_bind_local_models();
        } else if (esp_ble_mesh_provisioner_get_local_app_key(
                       ESP_BLE_MESH_NET_PRIMARY, APP_KEY_IDX) != NULL) {
            /* The add was refused because the key already lives in the stack,
             * restored from NVS by an earlier gateway run. The bindings are not
             * restored with it, so redo them or every send would fail with an
             * unbound client after a reboot. */
            ESP_LOGI(TAG, "app key already present, rebinding local models");
            prov_key.app_idx = APP_KEY_IDX;
            gateway_bind_local_models();
        }
        break;
    case ESP_BLE_MESH_PROVISIONER_BIND_APP_KEY_TO_MODEL_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROVISIONER_BIND_APP_KEY_TO_MODEL_COMP_EVT, err_code %d",
            param->provisioner_bind_app_key_to_model_comp.err_code);
        break;

    /* ---- Node events ---- */
    case ESP_BLE_MESH_PROV_REGISTER_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_PROV_REGISTER_COMP_EVT, err_code %d", param->prov_register_comp.err_code);
        break;
    case ESP_BLE_MESH_NODE_PROV_ENABLE_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_NODE_PROV_ENABLE_COMP_EVT, err_code %d", param->node_prov_enable_comp.err_code);
        break;
    case ESP_BLE_MESH_NODE_PROV_LINK_OPEN_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_NODE_PROV_LINK_OPEN_EVT, bearer %s",
            param->node_prov_link_open.bearer == ESP_BLE_MESH_PROV_ADV ? "PB-ADV" : "PB-GATT");
        break;
    case ESP_BLE_MESH_NODE_PROV_LINK_CLOSE_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_NODE_PROV_LINK_CLOSE_EVT, bearer %s",
            param->node_prov_link_close.bearer == ESP_BLE_MESH_PROV_ADV ? "PB-ADV" : "PB-GATT");
        break;
    case ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT");
        prov_complete(param->node_prov_complete.net_idx, param->node_prov_complete.addr,
            param->node_prov_complete.flags, param->node_prov_complete.iv_index);
        break;
    case ESP_BLE_MESH_NODE_PROV_RESET_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_NODE_PROV_RESET_EVT");
        break;
    case ESP_BLE_MESH_NODE_SET_UNPROV_DEV_NAME_COMP_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_NODE_SET_UNPROV_DEV_NAME_COMP_EVT, err_code %d", param->node_set_unprov_dev_name_comp.err_code);
        break;
    default:
        break;
    }
}

static void example_ble_mesh_generic_server_cb(esp_ble_mesh_generic_server_cb_event_t event,
                                               esp_ble_mesh_generic_server_cb_param_t *param)
{
    esp_ble_mesh_gen_onoff_srv_t *srv;
    ESP_LOGI(TAG, "event 0x%02x, opcode 0x%04" PRIx32 ", src 0x%04x, dst 0x%04x",
        event, param->ctx.recv_op, param->ctx.addr, param->ctx.recv_dst);

    switch (event) {
    case ESP_BLE_MESH_GENERIC_SERVER_STATE_CHANGE_EVT:

        /**
         * incomming message from mesh node
         */


        ESP_LOGI(TAG, "ESP_BLE_MESH_GENERIC_SERVER_STATE_CHANGE_EVT");
        if (param->ctx.recv_op == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET ||
            param->ctx.recv_op == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK) {
            ESP_LOGI(TAG, "onoff 0x%02x", param->value.state_change.onoff_set.onoff);
            example_change_led_state(param->model, &param->ctx, param->value.state_change.onoff_set.onoff);
        }
        break;
    case ESP_BLE_MESH_GENERIC_SERVER_RECV_GET_MSG_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_GENERIC_SERVER_RECV_GET_MSG_EVT");
        if (param->ctx.recv_op == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_GET) {
            srv = param->model->user_data;
            ESP_LOGI(TAG, "onoff 0x%02x", srv->state.onoff);
            example_handle_gen_onoff_msg(param->model, &param->ctx, NULL);
        }
        break;
    case ESP_BLE_MESH_GENERIC_SERVER_RECV_SET_MSG_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_GENERIC_SERVER_RECV_SET_MSG_EVT");
        if (param->ctx.recv_op == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET ||
            param->ctx.recv_op == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET_UNACK) {
            ESP_LOGI(TAG, "onoff 0x%02x, tid 0x%02x", param->value.set.onoff.onoff, param->value.set.onoff.tid);
            if (param->value.set.onoff.op_en) {
                ESP_LOGI(TAG, "trans_time 0x%02x, delay 0x%02x",
                    param->value.set.onoff.trans_time, param->value.set.onoff.delay);
            }
            example_handle_gen_onoff_msg(param->model, &param->ctx, &param->value.set.onoff);
        }
        break;
    default:
        ESP_LOGE(TAG, "Unknown Generic Server event 0x%02x", event);
        break;
    }
}


/**
 * @brief Example callback function for Generic OnOff Client model
 *
 * This function is called when:
 * - A Generic OnOff Status message is received (from a server)
 * - A response to a previous Get/Set operation arrives
 * - An unsolicited publish (status update) from a server arrives
 *
 * @param event One of esp_ble_mesh_generic_client_cb_event_t
 * @param param Pointer to event parameters
 */
static void example_ble_mesh_generic_client_cb(esp_ble_mesh_generic_client_cb_event_t event,
                                               esp_ble_mesh_generic_client_cb_param_t *param)
{
    ESP_LOGI(TAG, "Generic Client event: 0x%02x, error_code: 0x%02x", event, param->error_code);

    if (param->error_code) {
        ESP_LOGE(TAG, "Error occurred during processing (0x%02x)", param->error_code);
        return;
    }

    uint32_t opcode = param->params->opcode;
    uint16_t src_addr = param->params->ctx.addr;

    ESP_LOGI(TAG, "Received from src=0x%04x, opcode=0x%06" PRIx32, src_addr, opcode);

    switch (event) {
        case ESP_BLE_MESH_GENERIC_CLIENT_GET_STATE_EVT:
        case ESP_BLE_MESH_GENERIC_CLIENT_SET_STATE_EVT:
        case ESP_BLE_MESH_GENERIC_CLIENT_PUBLISH_EVT:
            if (opcode == ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_STATUS) {
                esp_ble_mesh_gen_onoff_status_cb_t status = param->status_cb.onoff_status;

                gateway_bridge_emitf("MSG src=0x%04x onoff=%s target=%s remain=0x%02x",
                    src_addr,
                    status.present_onoff ? "ON" : "OFF",
                    status.target_onoff  ? "ON" : "OFF",
                    status.remain_time);
            } else {
                ESP_LOGW(TAG, "Unexpected opcode: 0x%06" PRIx32, opcode);
            }
            break;

        default:
            ESP_LOGW(TAG, "Unhandled event: 0x%02x", event);
            break;
    }
}



static void example_ble_mesh_config_server_cb(esp_ble_mesh_cfg_server_cb_event_t event,
                                              esp_ble_mesh_cfg_server_cb_param_t *param)
{
    if (event == ESP_BLE_MESH_CFG_SERVER_STATE_CHANGE_EVT) {
        switch (param->ctx.recv_op) {
        case ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD:
            ESP_LOGI(TAG, "ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD");
            ESP_LOGI(TAG, "net_idx 0x%04x, app_idx 0x%04x",
                param->value.state_change.appkey_add.net_idx,
                param->value.state_change.appkey_add.app_idx);
            ESP_LOG_BUFFER_HEX("AppKey", param->value.state_change.appkey_add.app_key, 16);
            break;
        case ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND:
            ESP_LOGI(TAG, "ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND");
            ESP_LOGI(TAG, "elem_addr 0x%04x, app_idx 0x%04x, cid 0x%04x, mod_id 0x%04x",
                param->value.state_change.mod_app_bind.element_addr,
                param->value.state_change.mod_app_bind.app_idx,
                param->value.state_change.mod_app_bind.company_id,
                param->value.state_change.mod_app_bind.model_id);
            break;
        case ESP_BLE_MESH_MODEL_OP_MODEL_SUB_ADD:
            ESP_LOGI(TAG, "ESP_BLE_MESH_MODEL_OP_MODEL_SUB_ADD");
            ESP_LOGI(TAG, "elem_addr 0x%04x, sub_addr 0x%04x, cid 0x%04x, mod_id 0x%04x",
                param->value.state_change.mod_sub_add.element_addr,
                param->value.state_change.mod_sub_add.sub_addr,
                param->value.state_change.mod_sub_add.company_id,
                param->value.state_change.mod_sub_add.model_id);
            break;
        default:
            break;
        }
    }
}

/**
 * @brief Config Client callback -- gateway role only.
 *
 * Fires when a node answers a configuration request the gateway sent while
 * bringing it into the network.
 */
/* GROUP_ADDR is the one attribute with a side effect beyond being reported:
 * changing it moves which multicast traffic this node actually receives. */
static void identity_changed(dev_attr_id_t id)
{
    if (id == DEV_ATTR_GROUP_ADDR) {
        mesh_attr_apply_group();
    }
}

/* Query results reach the host on the same links commands arrive on. */
static void attr_status_to_host(uint16_t src_addr, const char *text)
{
    gateway_bridge_emitf("ATTR 0x%04x%s", src_addr, text);
}

static void attr_set_status_to_host(uint16_t src_addr, uint8_t result)
{
    static const char *reason[] = { "OK", "UNKNOWN_ATTR", "READ_ONLY", "BAD_VALUE" };
    gateway_bridge_emitf("ATTRSET 0x%04x %s", src_addr,
        result < (sizeof(reason) / sizeof(reason[0])) ? reason[result] : "ERR");
}

/* A SEND from the gateway landed here. Unlike the bridge lines this is not
 * gateway-gated: the text is addressed to THIS device, so it goes out to
 * whatever host or monitor is attached, in either role. */
static void attr_text_to_host(uint16_t src_addr, const char *text)
{
    char line[MESH_ATTR_MSG_MAX_LEN + 16];
    snprintf(line, sizeof(line), "TEXT 0x%04x %s", src_addr, text);
    uart_cmd_emit(line);
    spi_cmd_emit(line);
}

/* Fills in the boilerplate for a Config Client request aimed at `unicast`.
 * Config messages are secured with the node's device key, not an app key, so
 * app_idx is left at zero here. */
static void config_client_common(esp_ble_mesh_client_common_param_t *common,
                                 uint16_t unicast, uint32_t opcode)
{
    memset(common, 0, sizeof(*common));
    common->opcode       = opcode;
    common->model        = config_client.model;
    common->ctx.net_idx  = prov_key.net_idx;
    common->ctx.app_idx  = 0x0000;
    common->ctx.addr     = unicast;
    common->ctx.send_ttl = ESP_BLE_MESH_TTL_DEFAULT;
    common->ctx.send_rel = false;
    common->msg_timeout  = 0;
    common->msg_role     = ROLE_PROVISIONER;
}

/* A freshly provisioned node holds a network key but no app key, so it cannot
 * decrypt anything the attribute model sends. The gateway walks it through the
 * two steps that fix that:
 *
 *     APP_KEY_ADD     -> give the node the network's app key
 *     MODEL_APP_BIND  -> bind that key to its attribute server model
 *
 * Composition Data is deliberately not fetched first. Every board in this
 * network runs this same firmware, so the model layout is already known --
 * revisit that if mixed firmware ever joins.
 */
static void gateway_configure_node(uint16_t unicast)
{
    esp_ble_mesh_client_common_param_t common;
    esp_ble_mesh_cfg_client_set_state_t set = {0};

    config_client_common(&common, unicast, ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD);
    set.app_key_add.net_idx = prov_key.net_idx;
    set.app_key_add.app_idx = prov_key.app_idx;
    memcpy(set.app_key_add.app_key, prov_key.app_key, sizeof(set.app_key_add.app_key));

    esp_err_t err = esp_ble_mesh_config_client_set_state(&common, &set);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "app key add to 0x%04x failed (err %d)", unicast, err);
    } else {
        ESP_LOGI(TAG, "configuring node 0x%04x: app key add sent", unicast);
    }
}

static void gateway_bind_attr_model(uint16_t unicast)
{
    esp_ble_mesh_client_common_param_t common;
    esp_ble_mesh_cfg_client_set_state_t set = {0};

    config_client_common(&common, unicast, ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND);
    set.model_app_bind.element_addr  = unicast;
    set.model_app_bind.model_app_idx = prov_key.app_idx;
    set.model_app_bind.model_id      = MESH_ATTR_MODEL_ID_SERVER;
    set.model_app_bind.company_id    = CID_ESP;

    esp_err_t err = esp_ble_mesh_config_client_set_state(&common, &set);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "attr model bind on 0x%04x failed (err %d)", unicast, err);
    } else {
        ESP_LOGI(TAG, "configuring node 0x%04x: attr model bind sent", unicast);
    }
}

static void example_ble_mesh_config_client_cb(esp_ble_mesh_cfg_client_cb_event_t event,
                                              esp_ble_mesh_cfg_client_cb_param_t *param)
{
    if (param->error_code != ESP_OK) {
        ESP_LOGE(TAG, "Config Client error 0x%02x (opcode 0x%04" PRIx32 ")",
            param->error_code, param->params ? param->params->opcode : 0);
        return;
    }

    uint16_t src = param->params->ctx.addr;
    uint32_t opcode = param->params->opcode;

    ESP_LOGI(TAG, "Config Client event 0x%02x, src 0x%04x, opcode 0x%04" PRIx32,
        event, src, opcode);

    /* Advance the commissioning chain as each step is acknowledged. Timeouts
     * arrive as ESP_BLE_MESH_CFG_CLIENT_TIMEOUT_EVT and simply stall the chain;
     * re-provisioning the node restarts it. */
    if (event == ESP_BLE_MESH_CFG_CLIENT_SET_STATE_EVT) {
        if (opcode == ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD) {
            gateway_bind_attr_model(src);
        } else if (opcode == ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND) {
            ESP_LOGI(TAG, "node 0x%04x is configured and queryable", src);
            gateway_bridge_emitf("READY unicast=0x%04x", src);
        }
    }
}

static esp_err_t ble_mesh_init(void)
{
    esp_err_t err = ESP_OK;

    esp_ble_mesh_register_prov_callback(example_ble_mesh_provisioning_cb);
    esp_ble_mesh_register_config_server_callback(example_ble_mesh_config_server_cb);
    esp_ble_mesh_register_config_client_callback(example_ble_mesh_config_client_cb);
    esp_ble_mesh_register_generic_server_callback(example_ble_mesh_generic_server_cb);
    esp_ble_mesh_register_generic_client_callback(example_ble_mesh_generic_client_cb);



    err = esp_ble_mesh_init(&provision, &composition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize mesh stack (err %d)", err);
        return err;
    }

    /* Enabling node provisioning claims the node role and stores it in NVS,
     * and the provisioner refuses to start while it is held (see
     * ble_mesh_apply_mode). So a device that will come up as gateway must not
     * take it; every other device stays provisionable by someone else. */
    if (device_mode_get() != DEVICE_MODE_GATEWAY) {
        err = esp_ble_mesh_node_prov_enable(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT);
        if (err != ESP_OK) {
            /* Keep going: the mesh stack itself is up, and the role juggling
             * in ble_mesh_apply_mode() may still recover. */
            ESP_LOGE(TAG, "Failed to enable mesh node (err %d)", err);
        }
    }





    /* Registers the custom-model callback, so it has to follow
     * esp_ble_mesh_init(). A node needs this to answer queries at all. */
    err = mesh_attr_model_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start attribute model (err %d)", err);
        return err;
    }
    mesh_attr_model_register_cbs(attr_status_to_host, attr_set_status_to_host,
                                 attr_text_to_host);
    /* Lets the ASK and SEND verbs on the host channel reach the mesh without
     * lib/cmd having to depend on lib/BLE. */
    cmd_proto_register_mesh(mesh_attr_get, mesh_attr_set, mesh_attr_send_text);
    dev_identity_register_change_cb(identity_changed);

    ESP_LOGI(TAG, "BLE Mesh stack initialized");

    board_led_operation(LED_G, LED_ON);

    return err;
}

static esp_timer_handle_t s_rand_timer = NULL;

static void random_delay(esp_timer_cb_t callback, void *arg)
{
    uint64_t us = (1000 + (esp_random() % 9001)) * 1000ULL;
    if (s_rand_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = callback,
            .arg      = arg,
            .name     = "rand_delay",
        };
        esp_timer_create(&timer_args, &s_rand_timer);
    }
    esp_timer_start_once(s_rand_timer, us);
}

static void broadcast_stop(void)
{
    if (s_rand_timer) {
        esp_timer_stop(s_rand_timer);
    }
}

static void broadcast_group_cb(void *arg)
{
    /* A gateway does not spam the group address -- it listens and bridges.
     * Checked here as well as at stop time so a switch that races the timer
     * cannot re-arm it. */
    if (device_mode_get() != DEVICE_MODE_NODE) {
        return;
    }

    uint8_t status_data[3] = { 0x01, 0x01, 0x00 };
    esp_ble_mesh_model_t *model = &extend_model_0[0];
    model->pub->publish_addr = 0xc000;
    esp_err_t err = esp_ble_mesh_model_publish(
        model,
        ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_STATUS,
        sizeof(status_data),
        status_data,
        ROLE_NODE
    );
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "broadcast to 0xc000 failed: %d", err);
    } else {
        ESP_LOGI(TAG, "broadcast to 0xc000 sent");
    }
    random_delay(broadcast_group_cb, NULL);
}

/**
 * @brief Enter NODE or GATEWAY behaviour on the live mesh stack.
 *
 * Registered with lib/mode via device_mode_register_apply_cb(), so it runs both
 * at boot (for the mode restored from NVS) and on every runtime switch.
 * Returning an error aborts the switch and leaves the previous mode in place.
 */
static esp_err_t ble_mesh_apply_mode(device_mode_t mode)
{
    esp_err_t err;

    if (mode == DEVICE_MODE_GATEWAY) {
        /* The stack allows one role at a time and stores it in NVS: with node
         * provisioning active (enabled at init, or restored after a reboot)
         * esp_ble_mesh_provisioner_prov_enable() is refused with "Mismatch
         * role". Drop the node role first. A device that already joined some
         * other network as a node has to leave it before it can provision. */
        if (esp_ble_mesh_node_is_provisioned()) {
            ESP_LOGW(TAG, "leaving current network to become gateway");
            esp_ble_mesh_node_local_reset();
        }
        err = esp_ble_mesh_node_prov_disable(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT);
        if (err != ESP_OK) {
            /* Not fatal: already disabled is the usual cause after a reboot
             * straight into gateway mode. */
            ESP_LOGW(TAG, "node prov disable: %s", esp_err_to_name(err));
        }

        /* Only report unprovisioned devices whose UUID starts 0xdd 0xdd, which
         * is what this firmware advertises as a node (see dev_uuid). */
        static const uint8_t match[2] = { 0xdd, 0xdd };
        err = esp_ble_mesh_provisioner_set_dev_uuid_match(match, sizeof(match), 0x0, false);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to set UUID match (err %d)", err);
            return err;
        }

        err = esp_ble_mesh_provisioner_prov_enable(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to enable provisioner (err %d)", err);
            return err;
        }

        /* Generate the app key once; re-enabling the gateway later reuses it.
         * The bind to the local clients happens on ADD_LOCAL_APP_KEY_COMP_EVT. */
        if (prov_key.app_idx != APP_KEY_IDX || prov_key.app_key[0] == 0) {
            prov_key.net_idx = ESP_BLE_MESH_NET_PRIMARY;
            prov_key.app_idx = APP_KEY_IDX;
            memset(prov_key.app_key, APP_KEY_OCTET, sizeof(prov_key.app_key));

            err = esp_ble_mesh_provisioner_add_local_app_key(prov_key.app_key,
                                                             prov_key.net_idx,
                                                             prov_key.app_idx);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "failed to add local app key (err %d)", err);
                return err;
            }
        }

        broadcast_stop();
        /* A provisioner owns PROV_OWN_ADDR from init, so unlike a node it can
         * join its group without waiting to be provisioned. */
        mesh_attr_apply_group();
        ESP_LOGI(TAG, "gateway active: provisioner enabled, broadcast stopped");
    } else {
        err = esp_ble_mesh_provisioner_prov_disable(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            /* INVALID_STATE just means it was never enabled -- fine at boot. */
            ESP_LOGE(TAG, "failed to disable provisioner (err %d)", err);
            return err;
        }

        /* Mirror of the gateway branch: reclaim the node role so the device
         * can be provisioned (again). Redundant at first boot, where init
         * already enabled it, and after a reboot into node mode. */
        err = esp_ble_mesh_node_prov_enable(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "node prov enable: %s", esp_err_to_name(err));
        }

        random_delay(broadcast_group_cb, NULL);
        ESP_LOGI(TAG, "node active: provisioner disabled, broadcast running");
    }

    return ESP_OK;
}

    /**************************************  BLE MAIN TASK  *****************/
void BLETask(void)
{

    esp_err_t err;

    ESP_LOGI(TAG, "Initializing...");

    board_init();

    /* NVS is brought up in app_main(), before device_mode_init() reads the
     * stored role. */

    err = bluetooth_init();
    if (err) {
        ESP_LOGE(TAG, "esp32_bluetooth_init failed (err %d)", err);
        return;
    }

    ble_mesh_get_dev_uuid(dev_uuid);

    /* Initialize the Bluetooth Mesh Subsystem */
    err = ble_mesh_init();
    if (err) {
        ESP_LOGE(TAG, "Bluetooth mesh init failed (err %d)", err);
        return;
    }

    /* Now that the stack is up, hand it to lib/mode and enter the role that was
     * restored from NVS. Every later switch comes back through the same path. */
    device_mode_register_apply_cb(ble_mesh_apply_mode);
    device_mode_apply_stored();

    for (;;) {
        vTaskDelay(10000 / portTICK_PERIOD_MS);
        ESP_LOGI(TAG, "BLE Mesh running as %s", device_mode_name(device_mode_get()));
    }


}

void ble_task_init(void)
{
    xTaskCreate((void *)BLETask, "BLETask", 10000, NULL, 12, NULL);
}