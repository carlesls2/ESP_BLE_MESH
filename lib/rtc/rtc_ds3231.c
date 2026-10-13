/* rtc_ds3231.c - DS3231 backend for rtc_dev_t */

#include "rtc_ds3231.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define TAG "DS3231"

#define DS3231_ADDR     0x68
#define I2C_TIMEOUT     pdMS_TO_TICKS(50)

#define REG_SECONDS     0x00   /* 0x00-0x06: sec min hour dow date month year */
#define REG_ALARM1      0x07   /* 0x07-0x0A: sec min hour date */
#define REG_CONTROL     0x0E
#define REG_STATUS      0x0F

#define CTRL_A1IE       (1u << 0)
#define CTRL_INTCN      (1u << 2)
#define STAT_A1F        (1u << 0)
#define STAT_OSF        (1u << 7)   /* oscillator stopped: time is not trustworthy */
#define MONTH_CENTURY   (1u << 7)

typedef struct {
    i2c_port_t port;
} ds3231_ctx_t;

static ds3231_ctx_t s_ctx;
static rtc_dev_t    s_dev;

static uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t bin2bcd(int v)     { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static esp_err_t reg_read(rtc_dev_t *dev, uint8_t reg, uint8_t *buf, size_t len)
{
    ds3231_ctx_t *c = dev->ctx;
    return i2c_master_write_read_device(c->port, DS3231_ADDR, &reg, 1, buf, len, I2C_TIMEOUT);
}

static esp_err_t reg_write(rtc_dev_t *dev, uint8_t reg, const uint8_t *buf, size_t len)
{
    ds3231_ctx_t *c = dev->ctx;
    uint8_t       frame[8];

    if (len + 1 > sizeof(frame)) {
        return ESP_ERR_INVALID_SIZE;
    }
    frame[0] = reg;
    memcpy(frame + 1, buf, len);
    return i2c_master_write_to_device(c->port, DS3231_ADDR, frame, len + 1, I2C_TIMEOUT);
}

static esp_err_t reg_update(rtc_dev_t *dev, uint8_t reg, uint8_t clear, uint8_t set)
{
    uint8_t v;
    esp_err_t err = reg_read(dev, reg, &v, 1);
    if (err != ESP_OK) {
        return err;
    }
    v = (uint8_t)((v & ~clear) | set);
    return reg_write(dev, reg, &v, 1);
}

static esp_err_t ds_get_time(rtc_dev_t *dev, time_t *out)
{
    uint8_t r[7];
    esp_err_t err = reg_read(dev, REG_SECONDS, r, sizeof(r));
    if (err != ESP_OK) {
        return err;
    }

    rtc_civil_t c = {
        .sec   = bcd2bin(r[0] & 0x7F),
        .min   = bcd2bin(r[1] & 0x7F),
        /* 24-hour mode is always written by set_time; bit 6 would mean 12h. */
        .hour  = bcd2bin(r[2] & 0x3F),
        .day   = bcd2bin(r[4] & 0x3F),
        .month = bcd2bin(r[5] & 0x1F),
        .year  = 2000 + bcd2bin(r[6]) + ((r[5] & MONTH_CENTURY) ? 100 : 0),
    };
    *out = rtc_civil_to_epoch(&c);
    return ESP_OK;
}

static esp_err_t ds_set_time(rtc_dev_t *dev, time_t t)
{
    rtc_civil_t c;
    rtc_epoch_to_civil(t, &c);
    if (c.year < 2000 || c.year > 2199) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Day of week, 1-7 with Monday = 1; 1970-01-01 was a Thursday. The chip
     * only increments it, so any consistent convention will do. */
    int dow = (int)(((t / 86400) + 3) % 7) + 1;

    uint8_t r[7] = {
        bin2bcd(c.sec),
        bin2bcd(c.min),
        bin2bcd(c.hour),
        (uint8_t)dow,
        bin2bcd(c.day),
        (uint8_t)(bin2bcd(c.month) | (c.year >= 2100 ? MONTH_CENTURY : 0)),
        bin2bcd(c.year % 100),
    };
    esp_err_t err = reg_write(dev, REG_SECONDS, r, sizeof(r));
    if (err != ESP_OK) {
        return err;
    }
    /* A fresh set makes the time trustworthy again. */
    return reg_update(dev, REG_STATUS, STAT_OSF, 0);
}

static bool ds_time_valid(rtc_dev_t *dev)
{
    uint8_t st;
    return reg_read(dev, REG_STATUS, &st, 1) == ESP_OK && !(st & STAT_OSF);
}

static esp_err_t ds_set_alarm(rtc_dev_t *dev, time_t when)
{
    rtc_civil_t c;
    rtc_epoch_to_civil(when, &c);

    /* A1M1-A1M4 all clear: match seconds, minutes, hours and date (DY/DT = 0).
     * A date match is good for alarms up to a month ahead, far beyond any
     * sleep this firmware schedules. */
    uint8_t a[4] = {
        bin2bcd(c.sec),
        bin2bcd(c.min),
        bin2bcd(c.hour),
        bin2bcd(c.day),
    };
    esp_err_t err = reg_write(dev, REG_ALARM1, a, sizeof(a));
    if (err == ESP_OK) {
        err = reg_update(dev, REG_STATUS, STAT_A1F, 0);   /* drop a stale match */
    }
    if (err == ESP_OK) {
        err = reg_update(dev, REG_CONTROL, 0, CTRL_INTCN | CTRL_A1IE);
    }
    return err;
}

static esp_err_t ds_clear_alarm(rtc_dev_t *dev)
{
    esp_err_t err = reg_update(dev, REG_CONTROL, CTRL_A1IE, CTRL_INTCN);
    if (err == ESP_OK) {
        err = reg_update(dev, REG_STATUS, STAT_A1F, 0);   /* releases INT */
    }
    return err;
}

static const rtc_dev_ops_t s_ops = {
    .backend     = RTC_BACKEND_DS3231,
    .name        = "DS3231",
    .get_time    = ds_get_time,
    .set_time    = ds_set_time,
    .time_valid  = ds_time_valid,
    .set_alarm   = ds_set_alarm,
    .clear_alarm = ds_clear_alarm,
};

esp_err_t rtc_ds3231_create(const rtc_ds3231_cfg_t *cfg, rtc_dev_t **out)
{
    i2c_config_t ic = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = cfg->sda_gpio,
        .scl_io_num       = cfg->scl_gpio,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = cfg->clk_hz ? cfg->clk_hz : 100000,
    };

    esp_err_t err = i2c_param_config(cfg->port, &ic);
    if (err == ESP_OK) {
        err = i2c_driver_install(cfg->port, I2C_MODE_MASTER, 0, 0, 0);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C setup failed: %s", esp_err_to_name(err));
        return err;
    }

    s_ctx.port     = cfg->port;
    s_dev.ops      = &s_ops;
    s_dev.ctx      = &s_ctx;
    s_dev.alarm_gpio = cfg->alarm_gpio;

    uint8_t st;
    if (reg_read(&s_dev, REG_STATUS, &st, 1) != ESP_OK) {
        ESP_LOGW(TAG, "no DS3231 at 0x%02x on SDA %d / SCL %d",
                 DS3231_ADDR, cfg->sda_gpio, cfg->scl_gpio);
        i2c_driver_delete(cfg->port);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "found on SDA %d / SCL %d, alarm on GPIO %d%s",
             cfg->sda_gpio, cfg->scl_gpio, cfg->alarm_gpio,
             (st & STAT_OSF) ? " -- oscillator had stopped, time invalid" : "");
    *out = &s_dev;
    return ESP_OK;
}
