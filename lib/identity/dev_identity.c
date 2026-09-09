/* dev_identity.c - NVS-backed device identity */

#include "dev_identity.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#define TAG "DEV_ID"

#define NVS_NAMESPACE "devid"

/* One cache slot per registry row, indexed the same way. Raw wire form, so the
 * mesh path can memcpy straight out of it. Dynamic rows own a slot too -- it
 * simply stays empty, which keeps every index in this file identical to the
 * registry's. */
typedef struct {
    uint8_t value[DEV_ATTR_MAX_VALUE_LEN];
    size_t  len;
} attr_slot_t;

static attr_slot_t              *s_slots = NULL;
static SemaphoreHandle_t         s_lock = NULL;
static dev_identity_change_cb_t  s_change_cb = NULL;

static int slot_index(dev_attr_id_t id)
{
    for (size_t i = 0; i < dev_attr_count(); i++) {
        if (dev_attr_at(i)->id == id) {
            return (int)i;
        }
    }
    return -1;
}

static esp_err_t slot_persist(const dev_attr_desc_t *desc, const uint8_t *value, size_t len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(handle, desc->nvs_key, value, len);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to persist %s: %s", desc->name, esp_err_to_name(err));
    }
    return err;
}

/* Seeds the slot from NVS, falling back to the table default when the key is
 * absent (factory-fresh) or holds something the descriptor no longer accepts
 * (e.g. max_len was reduced in a later build). */
static void slot_load(size_t index)
{
    const dev_attr_desc_t *desc = dev_attr_at(index);
    attr_slot_t *slot = &s_slots[index];

    /* Dynamic rows have no nvs_key and no default -- nothing to load. */
    if (dev_attr_is_dynamic(desc)) {
        slot->len = 0;
        return;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t len = sizeof(slot->value);
        err = nvs_get_blob(handle, desc->nvs_key, slot->value, &len);
        nvs_close(handle);

        if (err == ESP_OK && len > 0 && len <= desc->max_len) {
            slot->len = len;
            return;
        }
    }

    slot->len = dev_attr_value_encode(desc, desc->def, slot->value, sizeof(slot->value));
    if (slot->len == 0) {
        ESP_LOGE(TAG, "default for %s is invalid; leaving empty", desc->name);
        return;
    }

    /* Write the default back so the value is readable by anything that inspects
     * NVS directly, and so first-boot state matches steady state. */
    slot_persist(desc, slot->value, slot->len);
    ESP_LOGI(TAG, "%s seeded with default", desc->name);
}

/* The single read funnel: a dynamic row is computed by its provider, a stored
 * one is copied out of the RAM cache. Providers are called with the lock
 * released -- they touch no state this module owns, and holding a mutex across
 * an ADC read would put the mesh receive path behind it. */
static bool fetch_raw(const dev_attr_desc_t *desc, int idx, uint8_t *out,
                      size_t out_size, size_t *out_len)
{
    if (dev_attr_is_dynamic(desc)) {
        return dev_attr_provide(desc, out, out_size, out_len);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = (s_slots[idx].len > 0) && (s_slots[idx].len <= out_size);
    if (ok) {
        memcpy(out, s_slots[idx].value, s_slots[idx].len);
        *out_len = s_slots[idx].len;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

void dev_identity_register_change_cb(dev_identity_change_cb_t cb)
{
    s_change_cb = cb;
}

esp_err_t dev_identity_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            ESP_LOGE(TAG, "failed to create identity mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_slots == NULL) {
        s_slots = calloc(dev_attr_count(), sizeof(attr_slot_t));
        if (s_slots == NULL) {
            ESP_LOGE(TAG, "failed to allocate attribute cache");
            return ESP_ERR_NO_MEM;
        }
    }

    for (size_t i = 0; i < dev_attr_count(); i++) {
        slot_load(i);
    }

    /* The firmware version belongs to the image, not to flash. Re-mirror it on
     * every boot so a board that was updated cannot keep reporting the version
     * it shipped with. */
    const dev_attr_desc_t *fw = dev_attr_find(DEV_ATTR_FW_VER);
    if (fw) {
        uint8_t built[DEV_ATTR_MAX_VALUE_LEN];
        size_t  built_len = dev_attr_value_encode(fw, fw->def, built, sizeof(built));
        int     idx = slot_index(DEV_ATTR_FW_VER);

        if (built_len > 0 && idx >= 0 &&
            (s_slots[idx].len != built_len ||
             memcmp(s_slots[idx].value, built, built_len) != 0)) {
            memcpy(s_slots[idx].value, built, built_len);
            s_slots[idx].len = built_len;
            slot_persist(fw, built, built_len);
            ESP_LOGI(TAG, "firmware version re-mirrored to %s", fw->def);
        }
    }

    /* Dynamic rows appear here only if telemetry_init() already registered
     * their providers; if it has not, they are simply quiet at boot and still
     * answer a later ID?. */
    for (size_t i = 0; i < dev_attr_count(); i++) {
        const dev_attr_desc_t *desc = dev_attr_at(i);
        uint8_t raw[DEV_ATTR_MAX_VALUE_LEN];
        size_t  raw_len = 0;
        char    text[DEV_ATTR_MAX_VALUE_LEN + 1];

        if (fetch_raw(desc, (int)i, raw, sizeof(raw), &raw_len) &&
            dev_attr_value_to_text(desc, raw, raw_len, text, sizeof(text))) {
            ESP_LOGI(TAG, "%-10s = %s", desc->name, text);
        }
    }

    return ESP_OK;
}

esp_err_t dev_identity_get_raw(dev_attr_id_t id, uint8_t *out, size_t out_size,
                               size_t *out_len)
{
    if (s_slots == NULL || out == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    int idx = slot_index(id);
    if (idx < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    const dev_attr_desc_t *desc = dev_attr_at((size_t)idx);
    if (!fetch_raw(desc, idx, out, out_size, out_len)) {
        /* A dynamic row whose provider declined is unavailable, not malformed:
         * no battery divider fitted, or no unicast address assigned yet. */
        return dev_attr_is_dynamic(desc) ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t dev_identity_get_text(dev_attr_id_t id, char *out, size_t out_size)
{
    const dev_attr_desc_t *desc = dev_attr_find(id);
    if (desc == NULL || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t raw[DEV_ATTR_MAX_VALUE_LEN];
    size_t  raw_len = 0;
    esp_err_t err = dev_identity_get_raw(id, raw, sizeof(raw), &raw_len);
    if (err != ESP_OK) {
        out[0] = '\0';
        return err;
    }

    return dev_attr_value_to_text(desc, raw, raw_len, out, out_size)
           ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

uint16_t dev_identity_group_addr(void)
{
    uint8_t raw[DEV_ATTR_MAX_VALUE_LEN];
    size_t  raw_len = 0;
    if (dev_identity_get_raw(DEV_ATTR_GROUP_ADDR, raw, sizeof(raw), &raw_len) != ESP_OK ||
        raw_len != 2) {
        return 0x0000;
    }
    return (uint16_t)raw[0] | ((uint16_t)raw[1] << 8);
}

esp_err_t dev_identity_set_raw(dev_attr_id_t id, const uint8_t *value,
                               size_t value_len, bool remote)
{
    const dev_attr_desc_t *desc = dev_attr_find(id);
    if (desc == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (s_slots == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Dynamic rows are computed; there is no store behind them to write. The
     * flag check below would refuse them anyway, but saying so plainly here
     * keeps the reason out of a confusing "not writable" log line. */
    if (dev_attr_is_dynamic(desc)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (value == NULL || value_len == 0 || value_len > desc->max_len) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t width = dev_attr_type_size(desc->type);
    if (width > 0 && value_len != width) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* A mesh write must not reach an attribute only the console owns. */
    bool allowed = remote ? dev_attr_writable_remote(desc) : dev_attr_writable_local(desc);
    if (!allowed) {
        ESP_LOGW(TAG, "%s is not writable %s", desc->name, remote ? "remotely" : "locally");
        return ESP_ERR_NOT_SUPPORTED;
    }

    int idx = slot_index(id);
    if (idx < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_slots[idx].value, value, value_len);
    s_slots[idx].len = value_len;
    esp_err_t err = slot_persist(desc, value, value_len);
    xSemaphoreGive(s_lock);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s updated (%s)", desc->name, remote ? "remote" : "local");
        if (s_change_cb) {
            s_change_cb(id);
        }
    }
    return err;
}

esp_err_t dev_identity_set_text(dev_attr_id_t id, const char *text, bool remote)
{
    const dev_attr_desc_t *desc = dev_attr_find(id);
    if (desc == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t raw[DEV_ATTR_MAX_VALUE_LEN];
    size_t  raw_len = dev_attr_value_encode(desc, text, raw, sizeof(raw));
    if (raw_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return dev_identity_set_raw(id, raw, raw_len, remote);
}

size_t dev_identity_encode_tlv(const dev_attr_id_t *ids, size_t id_count,
                               uint8_t *out, size_t out_size)
{
    if (out == NULL || s_slots == NULL) {
        return 0;
    }

    size_t offset = 0;
    size_t n = (ids == NULL || id_count == 0) ? dev_attr_count() : id_count;

    for (size_t i = 0; i < n; i++) {
        dev_attr_id_t id = (ids == NULL || id_count == 0) ? dev_attr_at(i)->id : ids[i];

        int idx = slot_index(id);
        if (idx < 0) {
            ESP_LOGW(TAG, "skipping unknown attribute 0x%02x", id);
            continue;
        }

        const dev_attr_desc_t *desc = dev_attr_at((size_t)idx);
        uint8_t raw[DEV_ATTR_MAX_VALUE_LEN];
        size_t  raw_len = 0;

        /* An unavailable dynamic value is omitted rather than sent as zero, so
         * the host can tell "no battery fitted" from "battery flat". */
        if (!fetch_raw(desc, idx, raw, sizeof(raw), &raw_len)) {
            continue;
        }

        if (!dev_attr_tlv_append(out, out_size, &offset, id, raw, raw_len)) {
            /* Out of room: return what fits rather than dropping the reply.
             * The requester can narrow the query and ask again. */
            ESP_LOGW(TAG, "TLV buffer full, truncating after %u bytes", (unsigned)offset);
            break;
        }
    }

    return offset;
}
