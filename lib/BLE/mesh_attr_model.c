/* mesh_attr_model.c - Vendor model carrying device attributes over the mesh */

#include "mesh_attr_model.h"

#include <stdio.h>
#include <string.h>

#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_local_data_operation_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "dev_identity.h"

#define TAG "ATTR_MDL"

#define CID_ESP 0x02E5

#define OP_ATTR_GET        ESP_BLE_MESH_MODEL_OP_3(MESH_ATTR_OP_B0_GET,        CID_ESP)
#define OP_ATTR_STATUS     ESP_BLE_MESH_MODEL_OP_3(MESH_ATTR_OP_B0_STATUS,     CID_ESP)
#define OP_ATTR_SET        ESP_BLE_MESH_MODEL_OP_3(MESH_ATTR_OP_B0_SET,        CID_ESP)
#define OP_ATTR_SET_STATUS ESP_BLE_MESH_MODEL_OP_3(MESH_ATTR_OP_B0_SET_STATUS, CID_ESP)

/* Longest id list a GET may carry. Past this, asking for everything (count 0)
 * is cheaper anyway. */
#define ATTR_MAX_REQUEST_IDS 8

/* Stagger window for replies to a group-addressed GET. Every subscriber hears
 * the request at the same instant, so without this they would all transmit
 * together and collide. */
#define STAGGER_MIN_MS 20
#define STAGGER_MAX_MS 500

#define REPLY_Q_DEPTH  4
#define REPLY_TASK_STK 3584
#define REPLY_TASK_PRI 9

/* Enough for the worst-case TLV reply holding every attribute. */
#define ATTR_MSG_BUF_LEN DEV_ATTR_MAX_TLV_LEN

typedef struct {
    uint16_t      dst;
    uint16_t      net_idx;
    uint16_t      app_idx;
    uint8_t       count;                     /* 0 = every attribute */
    bool          stagger;
    dev_attr_id_t ids[ATTR_MAX_REQUEST_IDS];
} reply_req_t;

static esp_ble_mesh_model_op_t s_server_ops[] = {
    ESP_BLE_MESH_MODEL_OP(OP_ATTR_GET, 1),
    ESP_BLE_MESH_MODEL_OP(OP_ATTR_SET, 3),
    ESP_BLE_MESH_MODEL_OP_END,
};

static esp_ble_mesh_model_op_t s_client_ops[] = {
    ESP_BLE_MESH_MODEL_OP(OP_ATTR_STATUS, 2),
    ESP_BLE_MESH_MODEL_OP(OP_ATTR_SET_STATUS, 1),
    ESP_BLE_MESH_MODEL_OP_END,
};

static const esp_ble_mesh_client_op_pair_t s_client_op_pair[] = {
    { OP_ATTR_GET, OP_ATTR_STATUS },
    { OP_ATTR_SET, OP_ATTR_SET_STATUS },
};

static esp_ble_mesh_client_t s_attr_client = {
    .op_pair_size = sizeof(s_client_op_pair) / sizeof(s_client_op_pair[0]),
    .op_pair      = s_client_op_pair,
};

esp_ble_mesh_model_t mesh_attr_vnd_models[MESH_ATTR_VND_MODEL_COUNT] = {
    ESP_BLE_MESH_VENDOR_MODEL(CID_ESP, MESH_ATTR_MODEL_ID_SERVER,
                              s_server_ops, NULL, NULL),
    ESP_BLE_MESH_VENDOR_MODEL(CID_ESP, MESH_ATTR_MODEL_ID_CLIENT,
                              s_client_ops, NULL, &s_attr_client),
};

#define MODEL_SERVER (&mesh_attr_vnd_models[0])
#define MODEL_CLIENT (&mesh_attr_vnd_models[1])

static QueueHandle_t             s_reply_q = NULL;
static mesh_attr_status_cb_t     s_status_cb = NULL;
static mesh_attr_set_status_cb_t s_set_status_cb = NULL;

void mesh_attr_model_register_cbs(mesh_attr_status_cb_t status_cb,
                                  mesh_attr_set_status_cb_t set_status_cb)
{
    s_status_cb = status_cb;
    s_set_status_cb = set_status_cb;
}

/* --- Server side: answering ---------------------------------------------- */

static void send_status(const reply_req_t *req)
{
    uint8_t body[ATTR_MSG_BUF_LEN];
    size_t  len = dev_identity_encode_tlv(req->count ? req->ids : NULL,
                                          req->count, body, sizeof(body));
    if (len == 0) {
        ESP_LOGW(TAG, "nothing to report to 0x%04x", req->dst);
        return;
    }

    esp_ble_mesh_msg_ctx_t ctx = {
        .net_idx  = req->net_idx,
        .app_idx  = req->app_idx,
        .addr     = req->dst,
        .send_ttl = ESP_BLE_MESH_TTL_DEFAULT,
        .send_rel = false,
    };

    esp_err_t err = esp_ble_mesh_server_model_send_msg(MODEL_SERVER, &ctx,
                                                      OP_ATTR_STATUS, len, body);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to send status to 0x%04x: %d", req->dst, err);
    } else {
        ESP_LOGI(TAG, "reported %u bytes to 0x%04x%s", (unsigned)len, req->dst,
                 len > 11 ? " (segmented)" : "");
    }
}

/* Replies leave from here rather than from the mesh callback, so a segmented
 * transmission never blocks the stack's own task and the stagger delay has
 * somewhere to sleep. */
static void reply_task(void *arg)
{
    reply_req_t req;

    for (;;) {
        if (xQueueReceive(s_reply_q, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (req.stagger) {
            uint32_t ms = STAGGER_MIN_MS +
                          (esp_random() % (STAGGER_MAX_MS - STAGGER_MIN_MS + 1));
            ESP_LOGI(TAG, "group query: replying in %u ms", (unsigned)ms);
            vTaskDelay(pdMS_TO_TICKS(ms));
        }
        send_status(&req);
    }
}

static void handle_get(esp_ble_mesh_msg_ctx_t *ctx, const uint8_t *msg, uint16_t len)
{
    reply_req_t req = {
        .dst     = ctx->addr,
        .net_idx = ctx->net_idx,
        .app_idx = ctx->app_idx,
        .count   = 0,
        /* recv_dst is the address the request was sent TO. Anything that is not
         * our own unicast means several nodes heard it at once. */
        .stagger = !ESP_BLE_MESH_ADDR_IS_UNICAST(ctx->recv_dst),
    };

    if (len >= 1) {
        uint8_t want = msg[0];
        if (want > 0) {
            if (want > ATTR_MAX_REQUEST_IDS || len < 1 + want) {
                ESP_LOGW(TAG, "malformed GET from 0x%04x (count %u, len %u)",
                         ctx->addr, want, len);
                return;
            }
            for (uint8_t i = 0; i < want; i++) {
                req.ids[i] = (dev_attr_id_t)msg[1 + i];
            }
            req.count = want;
        }
    }

    if (xQueueSend(s_reply_q, &req, 0) != pdTRUE) {
        ESP_LOGW(TAG, "reply queue full, dropping query from 0x%04x", ctx->addr);
    }
}

static void handle_set(esp_ble_mesh_msg_ctx_t *ctx, const uint8_t *msg, uint16_t len)
{
    uint8_t        result = MESH_ATTR_RESULT_OK;
    size_t         offset = 0;
    dev_attr_id_t  id;
    const uint8_t *value;
    size_t         value_len;
    unsigned       applied = 0;

    while (dev_attr_tlv_next(msg, len, &offset, &id, &value, &value_len)) {
        esp_err_t err = dev_identity_set_raw(id, value, value_len, true);
        if (err == ESP_OK) {
            applied++;
            continue;
        }

        /* First failure decides the reported result; keep going so a partially
         * valid write still applies what it can. */
        if (result == MESH_ATTR_RESULT_OK) {
            if (err == ESP_ERR_NOT_SUPPORTED)  result = MESH_ATTR_RESULT_READ_ONLY;
            else if (err == ESP_ERR_NOT_FOUND) result = MESH_ATTR_RESULT_UNKNOWN;
            else                               result = MESH_ATTR_RESULT_BAD_VALUE;
        }
        ESP_LOGW(TAG, "remote set of 0x%02x refused: %s", id, esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "remote set from 0x%04x applied %u attribute(s), result 0x%02x",
             ctx->addr, applied, result);

    esp_ble_mesh_msg_ctx_t rsp = {
        .net_idx  = ctx->net_idx,
        .app_idx  = ctx->app_idx,
        .addr     = ctx->addr,
        .send_ttl = ESP_BLE_MESH_TTL_DEFAULT,
        .send_rel = false,
    };
    esp_ble_mesh_server_model_send_msg(MODEL_SERVER, &rsp, OP_ATTR_SET_STATUS,
                                       sizeof(result), &result);
}

/* --- Client side: receiving answers -------------------------------------- */

/* Renders a TLV reply as one host-readable line: NAME=x FW_VER=y ... */
static void handle_status(esp_ble_mesh_msg_ctx_t *ctx, const uint8_t *msg, uint16_t len)
{
    char           line[224];
    size_t         used = 0;
    size_t         offset = 0;
    dev_attr_id_t  id;
    const uint8_t *value;
    size_t         value_len;

    line[0] = '\0';

    while (dev_attr_tlv_next(msg, len, &offset, &id, &value, &value_len)) {
        const dev_attr_desc_t *desc = dev_attr_find(id);
        char text[DEV_ATTR_MAX_VALUE_LEN + 1];

        if (desc == NULL) {
            /* A node running a newer table than ours. Report the raw id rather
             * than dropping it, so the mismatch is visible. */
            int n = snprintf(line + used, sizeof(line) - used, " 0x%02x=?", id);
            if (n > 0 && (size_t)n < sizeof(line) - used) {
                used += n;
            }
            continue;
        }
        if (!dev_attr_value_to_text(desc, value, value_len, text, sizeof(text))) {
            continue;
        }
        int n = snprintf(line + used, sizeof(line) - used, " %s=%s", desc->name, text);
        if (n <= 0 || (size_t)n >= sizeof(line) - used) {
            break;
        }
        used += n;
    }

    ESP_LOGI(TAG, "attrs from 0x%04x:%s", ctx->addr, line);
    if (s_status_cb) {
        s_status_cb(ctx->addr, line);
    }
}

static void model_cb(esp_ble_mesh_model_cb_event_t event,
                     esp_ble_mesh_model_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_MODEL_OPERATION_EVT: {
        uint32_t                opcode = param->model_operation.opcode;
        esp_ble_mesh_msg_ctx_t *ctx    = param->model_operation.ctx;
        uint8_t                *msg    = param->model_operation.msg;
        uint16_t                len    = param->model_operation.length;

        if (opcode == OP_ATTR_GET) {
            handle_get(ctx, msg, len);
        } else if (opcode == OP_ATTR_SET) {
            handle_set(ctx, msg, len);
        } else if (opcode == OP_ATTR_STATUS) {
            handle_status(ctx, msg, len);
        } else if (opcode == OP_ATTR_SET_STATUS) {
            uint8_t result = (len >= 1) ? msg[0] : MESH_ATTR_RESULT_BAD_VALUE;
            ESP_LOGI(TAG, "set status from 0x%04x: 0x%02x", ctx->addr, result);
            if (s_set_status_cb) {
                s_set_status_cb(ctx->addr, result);
            }
        }
        break;
    }
    case ESP_BLE_MESH_MODEL_SEND_COMP_EVT:
        if (param->model_send_comp.err_code != ESP_OK) {
            ESP_LOGE(TAG, "send failed for opcode 0x%06x, err %d",
                     (unsigned)param->model_send_comp.opcode,
                     param->model_send_comp.err_code);
        }
        break;
    default:
        break;
    }
}

esp_err_t mesh_attr_model_init(void)
{
    if (s_reply_q == NULL) {
        s_reply_q = xQueueCreate(REPLY_Q_DEPTH, sizeof(reply_req_t));
        if (s_reply_q == NULL) {
            ESP_LOGE(TAG, "failed to create reply queue");
            return ESP_ERR_NO_MEM;
        }
    }

    if (xTaskCreate(reply_task, "attr_reply", REPLY_TASK_STK, NULL,
                    REPLY_TASK_PRI, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create reply task");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_ble_mesh_register_custom_model_callback(model_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to register custom model callback: %d", err);
        return err;
    }

    ESP_LOGI(TAG, "attribute model ready (%u attributes)", (unsigned)dev_attr_count());
    return ESP_OK;
}

/* --- Group membership ---------------------------------------------------- */

/* Tracked so a change can drop the old subscription; the stack has no "replace"
 * operation and leaving the previous one in place would silently widen what the
 * node listens to. */
static uint16_t s_current_group = 0x0000;

esp_err_t mesh_attr_apply_group(void)
{
    if (MODEL_SERVER->element == NULL) {
        /* Not provisioned yet: no element address to subscribe with. The retry
         * comes from prov-complete calling this again. */
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t element_addr = MODEL_SERVER->element->element_addr;
    uint16_t want = dev_identity_group_addr();

    if (want == s_current_group) {
        return ESP_OK;
    }

    if (s_current_group != 0x0000) {
        esp_err_t err = esp_ble_mesh_model_unsubscribe_group_addr(
            element_addr, CID_ESP, MESH_ATTR_MODEL_ID_SERVER, s_current_group);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "failed to leave group 0x%04x: %d", s_current_group, err);
        } else {
            ESP_LOGI(TAG, "left group 0x%04x", s_current_group);
        }
    }

    s_current_group = 0x0000;

    if (want == 0x0000) {
        ESP_LOGI(TAG, "group unassigned");
        return ESP_OK;
    }

    /* Only 0xC000-0xFEFF are valid group addresses; anything else would be
     * rejected by the stack with a less obvious error. */
    if (!ESP_BLE_MESH_ADDR_IS_GROUP(want)) {
        ESP_LOGE(TAG, "0x%04x is not a group address (expected 0xC000-0xFEFF)", want);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = esp_ble_mesh_model_subscribe_group_addr(
        element_addr, CID_ESP, MESH_ATTR_MODEL_ID_SERVER, want);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to join group 0x%04x: %d", want, err);
        return err;
    }

    s_current_group = want;
    ESP_LOGI(TAG, "joined group 0x%04x", want);
    return ESP_OK;
}

/* --- Gateway side: asking ------------------------------------------------ */

static esp_err_t client_send(uint16_t dst, uint32_t opcode,
                             uint8_t *data, uint16_t len, bool need_rsp)
{
    /* element is filled in by the stack during esp_ble_mesh_init(); keys[0]
     * only leaves UNUSED once an app key has been bound to the model. Both are
     * ordinary states while a gateway is still coming up, so say which one is
     * missing rather than letting the send fail opaquely. */
    if (MODEL_CLIENT->element == NULL) {
        ESP_LOGE(TAG, "mesh stack not initialized yet");
        return ESP_ERR_INVALID_STATE;
    }
    if (MODEL_CLIENT->keys[0] == ESP_BLE_MESH_KEY_UNUSED) {
        ESP_LOGE(TAG, "no app key bound to the attribute client "
                      "(promote to gateway first)");
        return ESP_ERR_INVALID_STATE;
    }

    esp_ble_mesh_msg_ctx_t ctx = {
        .net_idx  = ESP_BLE_MESH_NET_PRIMARY,
        .app_idx  = MODEL_CLIENT->keys[0],
        .addr     = dst,
        .send_ttl = ESP_BLE_MESH_TTL_DEFAULT,
        .send_rel = false,
    };

    /* A group request has no single responder, so never wait for one -- each
     * node answers unicast on its own schedule and arrives as a STATUS. */
    bool unicast = ESP_BLE_MESH_ADDR_IS_UNICAST(dst);

    return esp_ble_mesh_client_model_send_msg(MODEL_CLIENT, &ctx, opcode,
                                              len, data,
                                              (unicast && need_rsp) ? 4000 : 0,
                                              unicast && need_rsp,
                                              ROLE_NODE);
}

esp_err_t mesh_attr_get(uint16_t dst, const dev_attr_id_t *ids, size_t id_count)
{
    if (id_count > ATTR_MAX_REQUEST_IDS) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t body[1 + ATTR_MAX_REQUEST_IDS];
    body[0] = (uint8_t)id_count;
    for (size_t i = 0; i < id_count; i++) {
        body[1 + i] = (uint8_t)ids[i];
    }

    ESP_LOGI(TAG, "querying 0x%04x for %s", dst,
             id_count ? "selected attributes" : "all attributes");

    return client_send(dst, OP_ATTR_GET, body, (uint16_t)(1 + id_count), true);
}

esp_err_t mesh_attr_set(uint16_t dst, dev_attr_id_t id, const char *text)
{
    const dev_attr_desc_t *desc = dev_attr_find(id);
    if (desc == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    /* Caught locally too, so a doomed write never reaches the air. */
    if (!dev_attr_writable_remote(desc)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t raw[DEV_ATTR_MAX_VALUE_LEN];
    size_t  raw_len = dev_attr_value_encode(desc, text, raw, sizeof(raw));
    if (raw_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t body[2 + DEV_ATTR_MAX_VALUE_LEN];
    size_t  offset = 0;
    if (!dev_attr_tlv_append(body, sizeof(body), &offset, id, raw, raw_len)) {
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "setting %s on 0x%04x", desc->name, dst);
    return client_send(dst, OP_ATTR_SET, body, (uint16_t)offset, true);
}
