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
#include "device_mode.h"
#include "neighbor.h"
#include "telemetry.h"

#define TAG "ATTR_MDL"

#define CID_ESP 0x02E5

#define OP_ATTR_GET        ESP_BLE_MESH_MODEL_OP_3(MESH_ATTR_OP_B0_GET,        CID_ESP)
#define OP_ATTR_STATUS     ESP_BLE_MESH_MODEL_OP_3(MESH_ATTR_OP_B0_STATUS,     CID_ESP)
#define OP_ATTR_SET        ESP_BLE_MESH_MODEL_OP_3(MESH_ATTR_OP_B0_SET,        CID_ESP)
#define OP_ATTR_SET_STATUS ESP_BLE_MESH_MODEL_OP_3(MESH_ATTR_OP_B0_SET_STATUS, CID_ESP)
#define OP_ATTR_MSG        ESP_BLE_MESH_MODEL_OP_3(MESH_ATTR_OP_B0_MSG,        CID_ESP)
#define OP_TELEM_GET       ESP_BLE_MESH_MODEL_OP_3(MESH_ATTR_OP_B0_TELEM_GET,    CID_ESP)
#define OP_TELEM_STATUS    ESP_BLE_MESH_MODEL_OP_3(MESH_ATTR_OP_B0_TELEM_STATUS, CID_ESP)
#define OP_NEIGH_GET       ESP_BLE_MESH_MODEL_OP_3(MESH_ATTR_OP_B0_NEIGH_GET,    CID_ESP)
#define OP_NEIGH_STATUS    ESP_BLE_MESH_MODEL_OP_3(MESH_ATTR_OP_B0_NEIGH_STATUS, CID_ESP)
#define OP_NEIGH_HELLO     ESP_BLE_MESH_MODEL_OP_3(MESH_ATTR_OP_B0_NEIGH_HELLO,  CID_ESP)
#define OP_RTC_SET         ESP_BLE_MESH_MODEL_OP_3(MESH_ATTR_OP_B0_RTC_SET,      CID_ESP)
#define OP_RTC_STATUS      ESP_BLE_MESH_MODEL_OP_3(MESH_ATTR_OP_B0_RTC_STATUS,   CID_ESP)

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

/* HELLO cadence: a random interval so boards powered up together drift apart
 * instead of colliding on every beat. */
#define HELLO_MIN_MS   8000
#define HELLO_MAX_MS   12000
#define HELLO_TASK_STK 2560
#define HELLO_TASK_PRI 5

/* Enough for the worst-case TLV reply holding every attribute. */
#define ATTR_MSG_BUF_LEN DEV_ATTR_MAX_TLV_LEN

/* Both query kinds ride the same reply queue: they share the stagger timing
 * and the need to transmit off the stack task. */
typedef enum {
    REPLY_KIND_ATTRS = 0,
    REPLY_KIND_TELEM = 1,
    REPLY_KIND_NEIGH = 2,
    REPLY_KIND_RTC   = 3,
} reply_kind_t;

typedef struct {
    reply_kind_t  kind;
    uint8_t       result;                    /* REPLY_KIND_RTC: RTC_RESULT_* */
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
    ESP_BLE_MESH_MODEL_OP(OP_ATTR_MSG, 1),
    ESP_BLE_MESH_MODEL_OP(OP_TELEM_GET, 1),
    ESP_BLE_MESH_MODEL_OP(OP_NEIGH_GET, 1),
    ESP_BLE_MESH_MODEL_OP(OP_NEIGH_HELLO, 1),
    ESP_BLE_MESH_MODEL_OP(OP_RTC_SET, sizeof(mesh_rtc_cmd_t)),
    ESP_BLE_MESH_MODEL_OP_END,
};

static esp_ble_mesh_model_op_t s_client_ops[] = {
    ESP_BLE_MESH_MODEL_OP(OP_ATTR_STATUS, 2),
    ESP_BLE_MESH_MODEL_OP(OP_ATTR_SET_STATUS, 1),
    ESP_BLE_MESH_MODEL_OP(OP_TELEM_STATUS, sizeof(mesh_telem_t)),
    ESP_BLE_MESH_MODEL_OP(OP_NEIGH_STATUS, sizeof(mesh_neigh_hdr_t)),
    ESP_BLE_MESH_MODEL_OP(OP_RTC_STATUS, sizeof(mesh_rtc_status_t)),
    ESP_BLE_MESH_MODEL_OP_END,
};

static const esp_ble_mesh_client_op_pair_t s_client_op_pair[] = {
    { OP_ATTR_GET, OP_ATTR_STATUS },
    { OP_ATTR_SET, OP_ATTR_SET_STATUS },
    { OP_TELEM_GET, OP_TELEM_STATUS },
    { OP_NEIGH_GET, OP_NEIGH_STATUS },
    { OP_RTC_SET, OP_RTC_STATUS },
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
static mesh_attr_text_cb_t       s_text_cb = NULL;
static mesh_attr_telem_cb_t      s_telem_cb = NULL;
static mesh_attr_neigh_cb_t      s_neigh_cb = NULL;
static mesh_attr_rtc_cb_t        s_rtc_cb = NULL;

void mesh_attr_model_register_rtc_cb(mesh_attr_rtc_cb_t rtc_cb)
{
    s_rtc_cb = rtc_cb;
}

void mesh_attr_model_register_cbs(mesh_attr_status_cb_t status_cb,
                                  mesh_attr_set_status_cb_t set_status_cb,
                                  mesh_attr_text_cb_t text_cb,
                                  mesh_attr_telem_cb_t telem_cb,
                                  mesh_attr_neigh_cb_t neigh_cb)
{
    s_status_cb = status_cb;
    s_set_status_cb = set_status_cb;
    s_text_cb = text_cb;
    s_telem_cb = telem_cb;
    s_neigh_cb = neigh_cb;
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

static void send_telem(const reply_req_t *req)
{
    telemetry_snapshot_t snap;
    telemetry_snapshot(&snap);

    mesh_telem_t body = {
        .ver           = MESH_TELEM_VER,
        .unicast       = snap.unicast,
        .uptime_s      = snap.uptime_s,
        .free_heap     = snap.free_heap,
        .min_free_heap = snap.min_free_heap,
        .batt_mv       = snap.batt_mv,
        .batt_pct      = snap.batt_pct,
        .rssi          = snap.rssi,
        .reset_reason  = snap.reset_reason,
        .flags         = (uint8_t)((snap.is_gateway ? MESH_TELEM_FLAG_GATEWAY    : 0) |
                                   (snap.batt_valid ? MESH_TELEM_FLAG_BATT_VALID : 0) |
                                   (snap.rssi_valid ? MESH_TELEM_FLAG_RSSI_VALID : 0)),
    };

    esp_ble_mesh_msg_ctx_t ctx = {
        .net_idx  = req->net_idx,
        .app_idx  = req->app_idx,
        .addr     = req->dst,
        .send_ttl = ESP_BLE_MESH_TTL_DEFAULT,
        .send_rel = false,
    };

    esp_err_t err = esp_ble_mesh_server_model_send_msg(MODEL_SERVER, &ctx,
                                                      OP_TELEM_STATUS,
                                                      sizeof(body), (uint8_t *)&body);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to send telemetry to 0x%04x: %d", req->dst, err);
    } else {
        ESP_LOGI(TAG, "telemetry sent to 0x%04x (%u bytes)", req->dst,
                 (unsigned)sizeof(body));
    }
}

static void send_neigh(const reply_req_t *req)
{
    neighbor_entry_t entries[NEIGH_MAX];
    size_t           n = neighbor_snapshot(entries, NEIGH_MAX);

    uint8_t          body[sizeof(mesh_neigh_hdr_t) + NEIGH_MAX * sizeof(mesh_neigh_wire_t)];
    mesh_neigh_hdr_t hdr = { .ver = MESH_NEIGH_VER, .count = (uint8_t)n };
    size_t           len = 0;

    memcpy(body, &hdr, sizeof(hdr));
    len += sizeof(hdr);
    for (size_t i = 0; i < n; i++) {
        mesh_neigh_wire_t w = {
            .addr      = entries[i].addr,
            .rssi_last = entries[i].rssi_last,
            .rssi_avg  = entries[i].rssi_avg,
            .age_s     = entries[i].age_s,
            .heard     = entries[i].heard,
        };
        memcpy(body + len, &w, sizeof(w));
        len += sizeof(w);
    }

    esp_ble_mesh_msg_ctx_t ctx = {
        .net_idx  = req->net_idx,
        .app_idx  = req->app_idx,
        .addr     = req->dst,
        .send_ttl = ESP_BLE_MESH_TTL_DEFAULT,
        .send_rel = false,
    };

    esp_err_t err = esp_ble_mesh_server_model_send_msg(MODEL_SERVER, &ctx, OP_NEIGH_STATUS,
                                                      (uint16_t)len, body);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to send neighbors to 0x%04x: %d", req->dst, err);
    } else {
        ESP_LOGI(TAG, "%u neighbor(s) sent to 0x%04x (%u bytes)", (unsigned)n, req->dst,
                 (unsigned)len);
    }
}

static void send_rtc_status(const reply_req_t *req)
{
    rtc_status_t st;
    rtc_mgr_status(&st);

    mesh_rtc_status_t body = {
        .ver        = MESH_RTC_VER,
        .result     = req->result,
        .valid      = st.valid,
        .backend    = (uint8_t)st.backend,
        .now        = (uint32_t)st.now,
        .wake       = (uint8_t)st.wake,
        .duty_on    = st.duty.enabled,
        .duty_mode  = (uint8_t)st.duty.mode,
        .duty_align = st.duty.align,
        .awake_s    = st.duty.awake_s,
        .sleep_s    = st.duty.sleep_s,
    };

    esp_ble_mesh_msg_ctx_t ctx = {
        .net_idx  = req->net_idx,
        .app_idx  = req->app_idx,
        .addr     = req->dst,
        .send_ttl = ESP_BLE_MESH_TTL_DEFAULT,
        .send_rel = false,
    };

    esp_err_t err = esp_ble_mesh_server_model_send_msg(MODEL_SERVER, &ctx, OP_RTC_STATUS,
                                                      sizeof(body), (uint8_t *)&body);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to send rtc status to 0x%04x: %d", req->dst, err);
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
        if (req.kind == REPLY_KIND_TELEM) {
            send_telem(&req);
        } else if (req.kind == REPLY_KIND_NEIGH) {
            send_neigh(&req);
        } else if (req.kind == REPLY_KIND_RTC) {
            send_rtc_status(&req);
        } else {
            send_status(&req);
        }
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

static void handle_telem_get(esp_ble_mesh_msg_ctx_t *ctx, const uint8_t *msg, uint16_t len)
{
    if (len >= 1 && msg[0] != MESH_TELEM_VER) {
        /* Answer anyway -- the requester checks the version in the reply and is
         * better placed to decide what to do about a mismatch than we are. */
        ESP_LOGW(TAG, "0x%04x asked for telemetry v%u, sending v%u",
                 ctx->addr, msg[0], MESH_TELEM_VER);
    }

    reply_req_t req = {
        .kind    = REPLY_KIND_TELEM,
        .dst     = ctx->addr,
        .net_idx = ctx->net_idx,
        .app_idx = ctx->app_idx,
        .count   = 0,
        .stagger = !ESP_BLE_MESH_ADDR_IS_UNICAST(ctx->recv_dst),
    };

    if (xQueueSend(s_reply_q, &req, 0) != pdTRUE) {
        ESP_LOGW(TAG, "reply queue full, dropping telemetry query from 0x%04x", ctx->addr);
    }
}

static void handle_neigh_get(esp_ble_mesh_msg_ctx_t *ctx, const uint8_t *msg, uint16_t len)
{
    if (len >= 1 && msg[0] != MESH_NEIGH_VER) {
        ESP_LOGW(TAG, "0x%04x asked for neighbors v%u, sending v%u",
                 ctx->addr, msg[0], MESH_NEIGH_VER);
    }

    reply_req_t req = {
        .kind    = REPLY_KIND_NEIGH,
        .dst     = ctx->addr,
        .net_idx = ctx->net_idx,
        .app_idx = ctx->app_idx,
        .stagger = !ESP_BLE_MESH_ADDR_IS_UNICAST(ctx->recv_dst),
    };

    if (xQueueSend(s_reply_q, &req, 0) != pdTRUE) {
        ESP_LOGW(TAG, "reply queue full, dropping neighbor query from 0x%04x", ctx->addr);
    }
}

/* Applied on the spot -- setting the clock or queueing a sleep is quick -- and
 * answered through the reply task like every other query. A queued sleep
 * waits RTC_SLEEP_GRACE_MS, which is what lets this answer get out first. */
static void handle_rtc_set(esp_ble_mesh_msg_ctx_t *ctx, const uint8_t *msg, uint16_t len)
{
    mesh_rtc_cmd_t cmd;
    memcpy(&cmd, msg, sizeof(cmd));

    uint8_t result;
    if (cmd.ver != MESH_RTC_VER) {
        ESP_LOGW(TAG, "0x%04x speaks rtc v%u, we speak v%u", ctx->addr, cmd.ver, MESH_RTC_VER);
        result = RTC_RESULT_FAILED;
    } else {
        rtc_req_t req = {
            .op       = (rtc_req_op_t)cmd.op,
            .mode     = cmd.mode ? RTC_SLEEP_DEEP : RTC_SLEEP_LIGHT,
            .absolute = cmd.absolute != 0,
            .a        = cmd.a,
            .b        = cmd.b,
        };
        result = rtc_mgr_result_code(rtc_mgr_apply(&req));
        ESP_LOGI(TAG, "rtc op %u from 0x%04x: %s", cmd.op, ctx->addr,
                 rtc_mgr_result_name(result));
    }

    reply_req_t rsp = {
        .kind    = REPLY_KIND_RTC,
        .result  = result,
        .dst     = ctx->addr,
        .net_idx = ctx->net_idx,
        .app_idx = ctx->app_idx,
        .stagger = !ESP_BLE_MESH_ADDR_IS_UNICAST(ctx->recv_dst),
    };
    if (xQueueSend(s_reply_q, &rsp, 0) != pdTRUE) {
        ESP_LOGW(TAG, "reply queue full, dropping rtc answer to 0x%04x", ctx->addr);
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

/* The wire carries the text with no terminator; rebuild a C string before
 * handing it up. Anything past the advertised maximum is truncated rather than
 * dropped, so a chatty sender still gets most of the line through. */
static void handle_msg(esp_ble_mesh_msg_ctx_t *ctx, const uint8_t *msg, uint16_t len)
{
    char text[MESH_ATTR_MSG_MAX_LEN + 1];

    if (len > MESH_ATTR_MSG_MAX_LEN) {
        len = MESH_ATTR_MSG_MAX_LEN;
    }
    memcpy(text, msg, len);
    text[len] = '\0';

    ESP_LOGI(TAG, "text from 0x%04x: %s", ctx->addr, text);
    if (s_text_cb) {
        s_text_cb(ctx->addr, text);
    }
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

static void handle_telem_status(esp_ble_mesh_msg_ctx_t *ctx, const uint8_t *msg,
                                uint16_t len)
{
    mesh_telem_t telem;

    if (len < sizeof(telem)) {
        ESP_LOGW(TAG, "short telemetry from 0x%04x (%u bytes, wanted %u)",
                 ctx->addr, len, (unsigned)sizeof(telem));
        return;
    }

    /* Copied out rather than cast in place: the receive buffer carries no
     * alignment guarantee for the 32-bit members. Both ends are little-endian
     * and the struct is packed, so the copy is the whole decode. */
    memcpy(&telem, msg, sizeof(telem));

    if (telem.ver != MESH_TELEM_VER) {
        ESP_LOGW(TAG, "0x%04x speaks telemetry v%u, we speak v%u -- ignoring",
                 ctx->addr, telem.ver, MESH_TELEM_VER);
        return;
    }

    ESP_LOGI(TAG, "telemetry from 0x%04x: up=%us heap=%u batt=%umV rssi=%d",
             ctx->addr, (unsigned)telem.uptime_s, (unsigned)telem.free_heap,
             (unsigned)telem.batt_mv, (int)telem.rssi);

    if (s_telem_cb) {
        s_telem_cb(ctx->addr, &telem);
    }
}

static void handle_neigh_status(esp_ble_mesh_msg_ctx_t *ctx, const uint8_t *msg,
                                uint16_t len)
{
    mesh_neigh_hdr_t  hdr;
    mesh_neigh_wire_t entries[NEIGH_MAX];

    /* The op table guarantees len >= sizeof(hdr). */
    memcpy(&hdr, msg, sizeof(hdr));
    if (hdr.ver != MESH_NEIGH_VER) {
        ESP_LOGW(TAG, "0x%04x speaks neighbors v%u, we speak v%u -- ignoring",
                 ctx->addr, hdr.ver, MESH_NEIGH_VER);
        return;
    }

    size_t count = hdr.count;
    size_t fit = (len - sizeof(hdr)) / sizeof(mesh_neigh_wire_t);
    if (count > fit) {
        ESP_LOGW(TAG, "0x%04x claims %u neighbors, carries %u",
                 ctx->addr, (unsigned)count, (unsigned)fit);
        count = fit;
    }
    if (count > NEIGH_MAX) {
        count = NEIGH_MAX;
    }
    /* Copied out: the receive buffer has no alignment guarantee. */
    memcpy(entries, msg + sizeof(hdr), count * sizeof(mesh_neigh_wire_t));

    ESP_LOGI(TAG, "%u neighbor(s) from 0x%04x", (unsigned)count, ctx->addr);
    if (s_neigh_cb) {
        s_neigh_cb(ctx->addr, entries, count);
    }
}

static void handle_rtc_status(esp_ble_mesh_msg_ctx_t *ctx, const uint8_t *msg,
                              uint16_t len)
{
    mesh_rtc_status_t w;
    memcpy(&w, msg, sizeof(w));     /* op table guarantees the length */

    if (w.ver != MESH_RTC_VER) {
        ESP_LOGW(TAG, "0x%04x speaks rtc v%u, we speak v%u -- ignoring",
                 ctx->addr, w.ver, MESH_RTC_VER);
        return;
    }

    rtc_status_t st = {
        .now     = (time_t)w.now,
        .valid   = w.valid != 0,
        .backend = (rtc_backend_t)w.backend,
        .wake    = (rtc_wake_cause_t)w.wake,
        .duty    = {
            .enabled = w.duty_on != 0,
            .mode    = w.duty_mode ? RTC_SLEEP_DEEP : RTC_SLEEP_LIGHT,
            .align   = w.duty_align != 0,
            .awake_s = w.awake_s,
            .sleep_s = w.sleep_s,
        },
    };
    if (s_rtc_cb) {
        s_rtc_cb(ctx->addr, &st, w.result);
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

        /* Every received message carries the radio's own measurement. Keeping
         * the latest here is what makes RSSI reportable at all -- nothing else
         * in the firmware ever sees an esp_ble_mesh_msg_ctx_t. */
        telemetry_note_rssi(ctx->recv_rssi);
        neighbor_note(ctx->addr, esp_ble_mesh_get_primary_element_address(),
                      ctx->recv_ttl, ctx->recv_rssi);

        if (opcode == OP_NEIGH_HELLO) {
            /* Noted above; carrying nothing else is the whole point. */
            ESP_LOGD(TAG, "hello from 0x%04x ttl %u rssi %d",
                     ctx->addr, ctx->recv_ttl, ctx->recv_rssi);
        } else if (opcode == OP_ATTR_GET) {
            handle_get(ctx, msg, len);
        } else if (opcode == OP_ATTR_SET) {
            handle_set(ctx, msg, len);
        } else if (opcode == OP_ATTR_MSG) {
            handle_msg(ctx, msg, len);
        } else if (opcode == OP_TELEM_GET) {
            handle_telem_get(ctx, msg, len);
        } else if (opcode == OP_TELEM_STATUS) {
            handle_telem_status(ctx, msg, len);
        } else if (opcode == OP_NEIGH_GET) {
            handle_neigh_get(ctx, msg, len);
        } else if (opcode == OP_NEIGH_STATUS) {
            handle_neigh_status(ctx, msg, len);
        } else if (opcode == OP_RTC_SET) {
            handle_rtc_set(ctx, msg, len);
        } else if (opcode == OP_RTC_STATUS) {
            handle_rtc_status(ctx, msg, len);
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
    case ESP_BLE_MESH_CLIENT_MODEL_SEND_TIMEOUT_EVT:
        /* Only NEIGH_GET reports its timeout: a gateway sweep waits for each
         * node in turn and should move on as soon as the stack gives up. */
        if (param->client_send_timeout.opcode == OP_NEIGH_GET && s_neigh_cb) {
            s_neigh_cb(param->client_send_timeout.ctx->addr, NULL, 0);
        } else if (param->client_send_timeout.opcode == OP_RTC_SET && s_rtc_cb) {
            s_rtc_cb(param->client_send_timeout.ctx->addr, NULL, RTC_RESULT_FAILED);
        }
        break;
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

/* --- Neighbor HELLO ------------------------------------------------------- */

/* TTL 0 to all-nodes: heard by every board in radio range and relayed by none.
 * A node sends with its server model, the only one the gateway bound an app key
 * to; a gateway keeps its key in the provisioner list, which only the client
 * send path can select. */
static void hello_send(void)
{
    bool                  gateway = (device_mode_get() == DEVICE_MODE_GATEWAY);
    esp_ble_mesh_model_t *model   = gateway ? MODEL_CLIENT : MODEL_SERVER;

    if (model->element == NULL || model->keys[0] == ESP_BLE_MESH_KEY_UNUSED) {
        return;     /* not provisioned / configured yet -- the normal early state */
    }

    uint8_t                body = MESH_NEIGH_VER;
    esp_ble_mesh_msg_ctx_t ctx = {
        .net_idx  = ESP_BLE_MESH_NET_PRIMARY,
        .app_idx  = model->keys[0],
        .addr     = ESP_BLE_MESH_ADDR_ALL_NODES,
        .send_ttl = 0,
        .send_rel = false,
    };

    esp_err_t err = gateway
        ? esp_ble_mesh_client_model_send_msg(model, &ctx, OP_NEIGH_HELLO, sizeof(body),
                                             &body, 0, false, ROLE_PROVISIONER)
        : esp_ble_mesh_server_model_send_msg(model, &ctx, OP_NEIGH_HELLO, sizeof(body),
                                             &body);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "hello failed: %d", err);
    }
}

static void hello_task(void *arg)
{
    for (;;) {
        uint32_t ms = HELLO_MIN_MS + (esp_random() % (HELLO_MAX_MS - HELLO_MIN_MS + 1));
        vTaskDelay(pdMS_TO_TICKS(ms));
        hello_send();
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

    if (xTaskCreate(hello_task, "neigh_hello", HELLO_TASK_STK, NULL,
                    HELLO_TASK_PRI, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create hello task");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_ble_mesh_register_custom_model_callback(model_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to register custom model callback: %d", err);
        return err;
    }

    /* Vendor client models are not initialized by the stack the way SIG
     * clients are: without this call the model has no internal data and
     * every send fails with "Invalid client internal data" (-EINVAL). */
    err = esp_ble_mesh_client_model_init(MODEL_CLIENT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to init attribute client model: %d", err);
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

    /* The stack keeps separate app-key lists per role. A gateway's key lives
     * in the provisioner list, so sending as ROLE_NODE there makes the key
     * lookup fail with "Invalid AppKeyIndex". */
    esp_ble_mesh_dev_role_t role =
        (device_mode_get() == DEVICE_MODE_GATEWAY) ? ROLE_PROVISIONER : ROLE_NODE;

    return esp_ble_mesh_client_model_send_msg(MODEL_CLIENT, &ctx, opcode,
                                              len, data,
                                              (unicast && need_rsp) ? 4000 : 0,
                                              unicast && need_rsp,
                                              role);
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

esp_err_t mesh_attr_send_text(uint16_t dst, const char *text)
{
    size_t len = text ? strlen(text) : 0;
    if (len == 0 || len > MESH_ATTR_MSG_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "sending %u chars to 0x%04x", (unsigned)len, dst);
    /* Fire-and-forget: no status opcode pairs with MSG, so never wait. */
    return client_send(dst, OP_ATTR_MSG, (uint8_t *)text, (uint16_t)len, false);
}

esp_err_t mesh_telem_get(uint16_t dst)
{
    uint8_t body = MESH_TELEM_VER;

    ESP_LOGI(TAG, "requesting telemetry from 0x%04x", dst);
    return client_send(dst, OP_TELEM_GET, &body, sizeof(body), true);
}

esp_err_t mesh_neigh_get(uint16_t dst)
{
    if (!ESP_BLE_MESH_ADDR_IS_UNICAST(dst)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t body = MESH_NEIGH_VER;

    ESP_LOGI(TAG, "requesting neighbors from 0x%04x", dst);
    return client_send(dst, OP_NEIGH_GET, &body, sizeof(body), true);
}

esp_err_t mesh_rtc_send(uint16_t dst, const rtc_req_t *req)
{
    mesh_rtc_cmd_t cmd = {
        .ver      = MESH_RTC_VER,
        .op       = (uint8_t)req->op,
        .mode     = (uint8_t)req->mode,
        .absolute = req->absolute,
        .a        = req->a,
        .b        = req->b,
    };

    ESP_LOGI(TAG, "rtc op %u to 0x%04x", cmd.op, dst);
    return client_send(dst, OP_RTC_SET, (uint8_t *)&cmd, sizeof(cmd), true);
}
