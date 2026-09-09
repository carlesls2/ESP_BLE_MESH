/* telemetry.c - Live device vitals */

#include "telemetry.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "esp_ble_mesh_local_data_operation_api.h"

#include "batt_adc.h"
#include "dev_attr.h"
#include "device_mode.h"

#define TAG "TELEM"

/* Written from the mesh receive task, read from whichever task is answering a
 * query. A single aligned byte pair is atomic on this core, so no lock: the
 * worst a racing reader sees is the previous message's value. */
static volatile int8_t s_rssi;
static volatile bool   s_rssi_valid;

void telemetry_note_rssi(int8_t rssi)
{
    s_rssi = rssi;
    s_rssi_valid = true;
}

const char *telemetry_reset_reason_str(uint8_t reason)
{
    switch ((esp_reset_reason_t)reason) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
    }
}

static uint32_t uptime_seconds(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

/* The stack returns 0x0000 (ESP_BLE_MESH_ADDR_UNASSIGNED) before provisioning,
 * and reads a plain static internally, so this is safe to call even before
 * esp_ble_mesh_init(). */
static uint16_t primary_addr(void)
{
    return esp_ble_mesh_get_primary_element_address();
}

void telemetry_snapshot(telemetry_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }

    out->uptime_s      = uptime_seconds();
    out->free_heap     = esp_get_free_heap_size();
    out->min_free_heap = esp_get_minimum_free_heap_size();
    out->reset_reason  = (uint8_t)esp_reset_reason();
    out->unicast       = primary_addr();
    out->is_gateway    = (device_mode_get() == DEVICE_MODE_GATEWAY);

    out->rssi       = s_rssi;
    out->rssi_valid = s_rssi_valid;

    out->batt_mv    = 0;
    out->batt_pct   = 0;
    out->batt_valid = batt_adc_read_mv(&out->batt_mv) &&
                      batt_adc_read_pct(&out->batt_pct);
}

/* --- Attribute providers -------------------------------------------------
 *
 * One per dynamic row. Each returns false when the value does not exist yet,
 * which makes dev_identity_encode_tlv() omit the attribute rather than send a
 * misleading zero.
 */

static bool provide_uptime(uint8_t *out, size_t out_size, size_t *out_len)
{
    return dev_attr_put_u32(uptime_seconds(), out, out_size, out_len);
}

static bool provide_heap(uint8_t *out, size_t out_size, size_t *out_len)
{
    return dev_attr_put_u32(esp_get_free_heap_size(), out, out_size, out_len);
}

static bool provide_heap_min(uint8_t *out, size_t out_size, size_t *out_len)
{
    return dev_attr_put_u32(esp_get_minimum_free_heap_size(), out, out_size, out_len);
}

static bool provide_reset(uint8_t *out, size_t out_size, size_t *out_len)
{
    return dev_attr_put_str(telemetry_reset_reason_str((uint8_t)esp_reset_reason()),
                            out, out_size, out_len);
}

static bool provide_unicast(uint8_t *out, size_t out_size, size_t *out_len)
{
    uint16_t addr = primary_addr();
    if (addr == 0x0000) {
        return false;   /* unprovisioned -- no address to report */
    }
    return dev_attr_put_u16(addr, out, out_size, out_len);
}

static bool provide_rssi(uint8_t *out, size_t out_size, size_t *out_len)
{
    if (!s_rssi_valid) {
        return false;   /* nothing heard yet */
    }
    return dev_attr_put_s8(s_rssi, out, out_size, out_len);
}

static bool provide_batt_mv(uint8_t *out, size_t out_size, size_t *out_len)
{
    uint16_t mv;
    if (!batt_adc_read_mv(&mv)) {
        return false;
    }
    return dev_attr_put_u16(mv, out, out_size, out_len);
}

static bool provide_batt_pct(uint8_t *out, size_t out_size, size_t *out_len)
{
    uint8_t pct;
    if (!batt_adc_read_pct(&pct)) {
        return false;
    }
    return dev_attr_put_u16(pct, out, out_size, out_len);
}

static bool provide_role(uint8_t *out, size_t out_size, size_t *out_len)
{
    return dev_attr_put_str(device_mode_name(device_mode_get()), out, out_size, out_len);
}

esp_err_t telemetry_init(void)
{
    esp_err_t err = batt_adc_init();
    if (err != ESP_OK) {
        /* A misconfigured divider must not take the whole device down: the
         * other vitals are still worth reporting. */
        ESP_LOGW(TAG, "battery ADC unavailable: %s", esp_err_to_name(err));
    }

    static const struct {
        dev_attr_id_t        id;
        dev_attr_provider_fn fn;
    } bindings[] = {
        { DEV_ATTR_UPTIME,   provide_uptime   },
        { DEV_ATTR_HEAP,     provide_heap     },
        { DEV_ATTR_HEAP_MIN, provide_heap_min },
        { DEV_ATTR_RESET,    provide_reset    },
        { DEV_ATTR_UNICAST,  provide_unicast  },
        { DEV_ATTR_RSSI,     provide_rssi     },
        { DEV_ATTR_BATT_MV,  provide_batt_mv  },
        { DEV_ATTR_BATT_PCT, provide_batt_pct },
        { DEV_ATTR_ROLE,     provide_role     },
    };

    for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
        if (!dev_attr_set_provider(bindings[i].id, bindings[i].fn)) {
            /* Only reachable if the registry table and this list fall out of
             * step -- worth shouting about, since the attribute would silently
             * report nothing forever. */
            ESP_LOGE(TAG, "no dynamic row for attribute 0x%02x", bindings[i].id);
        }
    }

    ESP_LOGI(TAG, "telemetry ready (reset=%s, battery=%s)",
             telemetry_reset_reason_str((uint8_t)esp_reset_reason()),
             batt_adc_present() ? "yes" : "not fitted");
    return ESP_OK;
}
