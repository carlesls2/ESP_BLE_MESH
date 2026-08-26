/* device_mode.c - Runtime NODE / GATEWAY personality */

#include "device_mode.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "blue_led.h"

#define TAG "DEV_MODE"

#define NVS_NAMESPACE "devcfg"
#define NVS_KEY_MODE  "mode"

/* Gateway advertises itself with a slow continuous blink; a node keeps the
 * blue LED dark. Reuses the non-blocking blinker in lib/board/blue_led.c. */
#define GATEWAY_BLINK_PERIOD_MS 1000

static device_mode_t          s_mode = DEVICE_MODE_NODE;
static device_mode_apply_cb_t s_apply_cb = NULL;
static SemaphoreHandle_t      s_lock = NULL;

static void mode_indicate(device_mode_t mode)
{
    if (mode == DEVICE_MODE_GATEWAY) {
        blue_led_blink(0, GATEWAY_BLINK_PERIOD_MS);
    } else {
        blue_led_blink_stop();
        blue_led_off();
    }
}

static esp_err_t mode_store(device_mode_t mode)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u8(handle, NVS_KEY_MODE, (uint8_t)mode);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to persist mode: %s", esp_err_to_name(err));
    }
    return err;
}

static device_mode_t mode_load(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        /* Namespace absent on a factory-fresh device -- not an error. */
        ESP_LOGI(TAG, "no stored mode (%s), defaulting to NODE", esp_err_to_name(err));
        return DEVICE_MODE_NODE;
    }

    uint8_t stored = DEVICE_MODE_NODE;
    err = nvs_get_u8(handle, NVS_KEY_MODE, &stored);
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no stored mode (%s), defaulting to NODE", esp_err_to_name(err));
        return DEVICE_MODE_NODE;
    }
    if (stored != DEVICE_MODE_NODE && stored != DEVICE_MODE_GATEWAY) {
        ESP_LOGW(TAG, "stored mode 0x%02x is invalid, defaulting to NODE", stored);
        return DEVICE_MODE_NODE;
    }
    return (device_mode_t)stored;
}

const char *device_mode_name(device_mode_t mode)
{
    switch (mode) {
    case DEVICE_MODE_NODE:    return "NODE";
    case DEVICE_MODE_GATEWAY: return "GATEWAY";
    default:                  return "UNKNOWN";
    }
}

void device_mode_register_apply_cb(device_mode_apply_cb_t cb)
{
    s_apply_cb = cb;
}

device_mode_t device_mode_get(void)
{
    return s_mode;
}

esp_err_t device_mode_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            ESP_LOGE(TAG, "failed to create mode mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    blue_led_init();

    s_mode = mode_load();
    mode_indicate(s_mode);

    ESP_LOGI(TAG, "boot mode: %s", device_mode_name(s_mode));
    return ESP_OK;
}

esp_err_t device_mode_apply_stored(void)
{
    if (s_apply_cb == NULL) {
        ESP_LOGW(TAG, "no apply callback registered");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = s_apply_cb(s_mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to apply boot mode %s: %s",
                 device_mode_name(s_mode), esp_err_to_name(err));
    }
    return err;
}

esp_err_t device_mode_set(device_mode_t mode)
{
    if (mode != DEVICE_MODE_NODE && mode != DEVICE_MODE_GATEWAY) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "device_mode_init() has not run");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    esp_err_t err = ESP_OK;

    if (mode == s_mode) {
        ESP_LOGI(TAG, "already in %s mode", device_mode_name(mode));
        goto out;
    }

    ESP_LOGI(TAG, "switching %s -> %s",
             device_mode_name(s_mode), device_mode_name(mode));

    if (s_apply_cb) {
        err = s_apply_cb(mode);
        if (err != ESP_OK) {
            /* Leave s_mode and NVS untouched so a failed promotion does not
             * strand the device in a role its mesh stack never entered. */
            ESP_LOGE(TAG, "apply failed (%s), staying in %s",
                     esp_err_to_name(err), device_mode_name(s_mode));
            goto out;
        }
    } else {
        ESP_LOGW(TAG, "no apply callback registered; mode change is cosmetic");
    }

    s_mode = mode;
    mode_indicate(s_mode);
    err = mode_store(s_mode);

    ESP_LOGI(TAG, "now running as %s", device_mode_name(s_mode));

out:
    xSemaphoreGive(s_lock);
    return err;
}
