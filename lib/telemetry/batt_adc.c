/* batt_adc.c - Battery voltage over an ADC1 divider (ESP-IDF 4.4 legacy API) */

#include "batt_adc.h"

#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "esp_log.h"

#define TAG "BATT"

/* eFuse-less fallback reference in mV. Boards fused at the factory (almost all
 * of them since 2018) carry their own Vref and this value is ignored. */
#define BATT_DEFAULT_VREF 1100

/* Averaged to damp the ESP32's notoriously noisy SAR ADC. 16 samples is about
 * 200 us and cuts the spread to a few mV. */
#define BATT_SAMPLES 16

#ifndef BATT_DIVIDER_NUM
#define BATT_DIVIDER_NUM 2
#endif
#ifndef BATT_DIVIDER_DEN
#define BATT_DIVIDER_DEN 1
#endif

#ifdef BATT_ADC_GPIO
static bool                          s_ready;
static esp_adc_cal_characteristics_t s_cal;


/* ESP32 ADC1 pin map. Anything else has no ADC1 channel and is rejected at
 * compile time rather than silently reading the wrong pin. */
static adc1_channel_t gpio_to_adc1(int gpio)
{
    switch (gpio) {
    case 36: return ADC1_CHANNEL_0;
    case 37: return ADC1_CHANNEL_1;
    case 38: return ADC1_CHANNEL_2;
    case 39: return ADC1_CHANNEL_3;
    case 32: return ADC1_CHANNEL_4;
    case 33: return ADC1_CHANNEL_5;
    case 34: return ADC1_CHANNEL_6;
    case 35: return ADC1_CHANNEL_7;
    default: return ADC1_CHANNEL_MAX;
    }
}
#endif

bool batt_adc_present(void)
{
#ifdef BATT_ADC_GPIO
    return s_ready;
#else
    return false;
#endif
}

esp_err_t batt_adc_init(void)
{
#ifndef BATT_ADC_GPIO
    ESP_LOGI(TAG, "no BATT_ADC_GPIO configured; battery reporting disabled");
    return ESP_OK;
#else
    adc1_channel_t ch = gpio_to_adc1(BATT_ADC_GPIO);
    if (ch == ADC1_CHANNEL_MAX) {
        ESP_LOGE(TAG, "GPIO%d is not an ADC1 pin (use 32-39); battery disabled",
                 (int)BATT_ADC_GPIO);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = adc1_config_width(ADC_WIDTH_BIT_12);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc1_config_width failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 11 dB attenuation puts full scale near 3.1 V, which is what a divided
     * LiPo lands in. Less attenuation would clip a full battery. */
    err = adc1_config_channel_atten(ch, ADC_ATTEN_DB_11);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc1_config_channel_atten failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_adc_cal_value_t src = esp_adc_cal_characterize(
        ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, BATT_DEFAULT_VREF, &s_cal);

    ESP_LOGI(TAG, "battery on GPIO%d, divider %d/%d, calibration: %s",
             (int)BATT_ADC_GPIO, (int)BATT_DIVIDER_NUM, (int)BATT_DIVIDER_DEN,
             src == ESP_ADC_CAL_VAL_EFUSE_VREF ? "eFuse Vref"
             : src == ESP_ADC_CAL_VAL_EFUSE_TP ? "eFuse two-point"
                                               : "default Vref (less accurate)");

    s_ready = true;
    return ESP_OK;
#endif
}

bool batt_adc_read_mv(uint16_t *mv)
{
#ifndef BATT_ADC_GPIO
    (void)mv;
    return false;
#else
    if (!s_ready || mv == NULL) {
        return false;
    }

    adc1_channel_t ch = gpio_to_adc1(BATT_ADC_GPIO);
    uint32_t acc = 0;
    for (int i = 0; i < BATT_SAMPLES; i++) {
        int raw = adc1_get_raw(ch);
        if (raw < 0) {
            return false;
        }
        acc += (uint32_t)raw;
    }

    uint32_t at_pin = esp_adc_cal_raw_to_voltage(acc / BATT_SAMPLES, &s_cal);
    uint32_t vbatt  = at_pin * BATT_DIVIDER_NUM / BATT_DIVIDER_DEN;

    *mv = (vbatt > UINT16_MAX) ? UINT16_MAX : (uint16_t)vbatt;
    return true;
#endif
}

/* Single-cell LiPo discharge curve. A linear 3.30-4.20 V map is badly wrong at
 * both ends -- the curve is flat through the middle and falls off a cliff below
 * 3.6 V -- so interpolate between measured points instead. */
static const struct { uint16_t mv; uint8_t pct; } s_curve[] = {
    { 4200, 100 }, { 4100, 95 }, { 4000, 87 }, { 3950, 80 },
    { 3900,  73 }, { 3850, 65 }, { 3800, 57 }, { 3780, 50 },
    { 3750,  43 }, { 3720, 35 }, { 3700, 28 }, { 3650, 20 },
    { 3600,  13 }, { 3500,  6 }, { 3400,  3 }, { 3300,  0 },
};

#define BATT_CURVE_LEN (sizeof(s_curve) / sizeof(s_curve[0]))

bool batt_adc_read_pct(uint8_t *pct)
{
    /* Initialised because the no-divider build of batt_adc_read_mv() returns
     * without writing through the pointer, which GCC cannot see past. */
    uint16_t mv = 0;
    if (pct == NULL || !batt_adc_read_mv(&mv)) {
        return false;
    }

    if (mv >= s_curve[0].mv) {
        *pct = 100;
        return true;
    }
    if (mv <= s_curve[BATT_CURVE_LEN - 1].mv) {
        *pct = 0;
        return true;
    }

    for (size_t i = 1; i < BATT_CURVE_LEN; i++) {
        if (mv >= s_curve[i].mv) {
            uint16_t hi_mv  = s_curve[i - 1].mv, lo_mv  = s_curve[i].mv;
            uint8_t  hi_pct = s_curve[i - 1].pct, lo_pct = s_curve[i].pct;
            uint32_t span   = hi_mv - lo_mv;
            *pct = (uint8_t)(lo_pct + ((uint32_t)(mv - lo_mv) * (hi_pct - lo_pct) + span / 2) / span);
            return true;
        }
    }

    *pct = 0;
    return true;
}
