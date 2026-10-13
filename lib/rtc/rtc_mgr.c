/* rtc_mgr.c - Time keeping and sleep scheduling */

#include "rtc_mgr.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "driver/rtc_io.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"

#include "device_mode.h"
#include "rtc_ds3231.h"

#define TAG "RTC"

#define NVS_NAMESPACE "rtc"
#define NVS_KEY_DUTY  "duty"
#define DUTY_BLOB_VER 1

/* Anything before this is an unset clock, not a real date. */
#define EPOCH_SANE_MIN 1577836800   /* 2020-01-01 */

/* Longest single sleep. The DS3231 alarm matches on day-of-month, so it must
 * stay well under a month; a week is plenty for a mesh node. */
#define SLEEP_MAX_S (7u * 24u * 3600u)

#define TASK_STK 3072
#define TASK_PRI 4

typedef enum {
    MSG_SLEEP,          /* one-shot sleep */
    MSG_DUTY_RESTART,   /* duty config changed: start a fresh awake window */
} msg_type_t;

typedef struct {
    msg_type_t       type;
    rtc_sleep_mode_t mode;
    uint32_t         seconds;
} rtc_msg_t;

/* NVS layout of the duty cycle. Versioned so a future field does not misread
 * an old blob. */
typedef struct __attribute__((packed)) {
    uint8_t  ver;
    uint8_t  enabled;
    uint8_t  mode;
    uint8_t  align;
    uint32_t awake_s;
    uint32_t sleep_s;
} duty_blob_t;

static rtc_dev_t        *s_dev;
static rtc_wake_cause_t  s_wake = RTC_WAKE_RESET;
static rtc_duty_t        s_duty;
static portMUX_TYPE      s_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t     s_q;

static bool is_gateway(void)
{
    return device_mode_get() == DEVICE_MODE_GATEWAY;
}

/* --- Duty persistence ---------------------------------------------------- */

static void duty_load(void)
{
    nvs_handle_t h;
    duty_blob_t  b;
    size_t       len = sizeof(b);

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;     /* nothing stored yet */
    }
    if (nvs_get_blob(h, NVS_KEY_DUTY, &b, &len) == ESP_OK &&
        len == sizeof(b) && b.ver == DUTY_BLOB_VER) {
        s_duty.enabled = b.enabled != 0;
        s_duty.mode    = b.mode ? RTC_SLEEP_DEEP : RTC_SLEEP_LIGHT;
        s_duty.align   = b.align != 0;
        s_duty.awake_s = b.awake_s;
        s_duty.sleep_s = b.sleep_s;
    }
    nvs_close(h);
}

static esp_err_t duty_save(const rtc_duty_t *d)
{
    duty_blob_t b = {
        .ver     = DUTY_BLOB_VER,
        .enabled = d->enabled,
        .mode    = (uint8_t)d->mode,
        .align   = d->align,
        .awake_s = d->awake_s,
        .sleep_s = d->sleep_s,
    };
    nvs_handle_t h;
    esp_err_t    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, NVS_KEY_DUTY, &b, sizeof(b));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* --- The sleep itself ---------------------------------------------------- */

static rtc_wake_cause_t map_wake(esp_sleep_wakeup_cause_t c)
{
    switch (c) {
    case ESP_SLEEP_WAKEUP_UNDEFINED: return RTC_WAKE_RESET;
    case ESP_SLEEP_WAKEUP_TIMER:     return RTC_WAKE_TIMER;
    case ESP_SLEEP_WAKEUP_EXT0:      return RTC_WAKE_ALARM;
    default:                         return RTC_WAKE_OTHER;
    }
}

static void do_sleep(rtc_sleep_mode_t mode, uint32_t seconds)
{
    if (is_gateway()) {
        ESP_LOGW(TAG, "gateway does not sleep -- request dropped");
        return;
    }

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    /* With an external alarm wired, it is the precise wake-up and the sleep
     * timer only a backstop in case the alarm never fires; 10 % late covers
     * the internal RC oscillator's error. Without one, the timer is it. */
    uint64_t timer_s = seconds;
    time_t   now;
    rtc_dev_clear_alarm(s_dev);
    if (rtc_dev_has_alarm(s_dev) && rtc_dev_time_valid(s_dev) &&
        rtc_dev_get_time(s_dev, &now) == ESP_OK &&
        rtc_dev_set_alarm(s_dev, now + seconds) == ESP_OK) {
        gpio_num_t pin = (gpio_num_t)s_dev->alarm_gpio;
        rtc_gpio_pullup_en(pin);
        rtc_gpio_pulldown_dis(pin);
        esp_sleep_enable_ext0_wakeup(pin, 0);
        timer_s = seconds + seconds / 10 + 5;
    }
    esp_sleep_enable_timer_wakeup(timer_s * 1000000ULL);

    ESP_LOGI(TAG, "%s sleep for %u s", rtc_mgr_mode_name(mode), (unsigned)seconds);
    /* Let the log line and any queued host reply leave the UART first. */
    fflush(stdout);
    uart_wait_tx_idle_polling(CONFIG_ESP_CONSOLE_UART_NUM);

    if (mode == RTC_SLEEP_DEEP) {
        esp_deep_sleep_start();     /* does not return: wake is a reboot */
    }

    esp_light_sleep_start();

    s_wake = map_wake(esp_sleep_get_wakeup_cause());
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    rtc_dev_clear_alarm(s_dev);
    ESP_LOGI(TAG, "awake from light sleep (%s)", rtc_mgr_wake_name(s_wake));
}

/* How long the next duty sleep lasts. Aligned: until the next wall-clock
 * multiple of the period, so every board with the same schedule wakes
 * together. Falls back to a plain sleep_s while the clock is unset. */
static uint32_t duty_sleep_seconds(const rtc_duty_t *d)
{
    time_t   now;
    uint32_t period = d->awake_s + d->sleep_s;

    if (!d->align || !rtc_dev_time_valid(s_dev) ||
        rtc_dev_get_time(s_dev, &now) != ESP_OK) {
        return d->sleep_s;
    }
    time_t   next = (now / period + 1) * period;
    uint32_t s = (uint32_t)(next - now);
    /* Too close to the boundary to be worth it: skip to the one after. */
    return s < 2 ? s + period : s;
}

static void rtc_task(void *arg)
{
    int64_t window_start = esp_timer_get_time();

    for (;;) {
        rtc_duty_t d;
        portENTER_CRITICAL(&s_lock);
        d = s_duty;
        portEXIT_CRITICAL(&s_lock);

        TickType_t wait = portMAX_DELAY;
        if (d.enabled) {
            int64_t left_us = (int64_t)d.awake_s * 1000000 -
                              (esp_timer_get_time() - window_start);
            wait = left_us > 0 ? pdMS_TO_TICKS(left_us / 1000) : 0;
        }

        rtc_msg_t m;
        if (xQueueReceive(s_q, &m, wait) == pdTRUE) {
            if (m.type == MSG_SLEEP) {
                vTaskDelay(pdMS_TO_TICKS(RTC_SLEEP_GRACE_MS));
                do_sleep(m.mode, m.seconds);
            }
            window_start = esp_timer_get_time();
            continue;
        }

        /* Awake window is over. A board promoted to gateway keeps its stored
         * schedule but does not act on it. */
        if (!is_gateway()) {
            do_sleep(d.mode, duty_sleep_seconds(&d));
        }
        window_start = esp_timer_get_time();
    }
}

/* --- Public API ---------------------------------------------------------- */

static rtc_dev_t *pick_backend(void)
{
#ifdef RTC_DS3231_SDA
#ifndef RTC_DS3231_SCL
#define RTC_DS3231_SCL 22
#endif
#ifndef RTC_ALARM_GPIO
#define RTC_ALARM_GPIO -1
#endif
    rtc_ds3231_cfg_t cfg = {
        .port       = I2C_NUM_0,
        .sda_gpio   = RTC_DS3231_SDA,
        .scl_gpio   = RTC_DS3231_SCL,
        .alarm_gpio = RTC_ALARM_GPIO,
        .clk_hz     = 100000,
    };
    rtc_dev_t *ds = NULL;
    if (rtc_ds3231_create(&cfg, &ds) == ESP_OK) {
        return ds;
    }
    ESP_LOGW(TAG, "DS3231 configured but not found -- using the internal RTC");
#endif
    return rtc_internal_get();
}

esp_err_t rtc_mgr_init(void)
{
    s_wake = map_wake(esp_sleep_get_wakeup_cause());
    s_dev = pick_backend();

    /* The chip is the master copy: bring the system clock in line with it.
     * After a deep sleep the system clock survived anyway, but the chip's
     * crystal is far better than the RC oscillator that kept it. */
    time_t t;
    if (s_dev->ops->backend != RTC_BACKEND_INTERNAL && rtc_dev_time_valid(s_dev) &&
        rtc_dev_get_time(s_dev, &t) == ESP_OK) {
        rtc_dev_set_time(rtc_internal_get(), t);
    }
    rtc_dev_clear_alarm(s_dev);

    duty_load();

    s_q = xQueueCreate(4, sizeof(rtc_msg_t));
    if (s_q == NULL ||
        xTaskCreate(rtc_task, "rtc_mgr", TASK_STK, NULL, TASK_PRI, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start the RTC task");
        return ESP_ERR_NO_MEM;
    }

    rtc_status_t st;
    char         line[128];
    rtc_mgr_status(&st);
    rtc_mgr_format_status(&st, line, sizeof(line));
    ESP_LOGI(TAG, "%s", line);
    return ESP_OK;
}

esp_err_t rtc_mgr_get_time(time_t *now, bool *valid)
{
    /* The system clock is always the one read: no I2C on the hot path. */
    esp_err_t err = rtc_dev_get_time(rtc_internal_get(), now);
    if (valid) {
        *valid = rtc_dev_time_valid(s_dev);
    }
    return err;
}

esp_err_t rtc_mgr_set_time(time_t t)
{
    if (t < EPOCH_SANE_MIN) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = rtc_dev_set_time(rtc_internal_get(), t);
    if (err == ESP_OK && s_dev != rtc_internal_get()) {
        err = rtc_dev_set_time(s_dev, t);
    }
    if (err == ESP_OK) {
        char iso[24];
        rtc_mgr_format_iso(t, iso, sizeof(iso));
        ESP_LOGI(TAG, "time set to %s", iso);
    }
    return err;
}

esp_err_t rtc_mgr_sleep_for(rtc_sleep_mode_t mode, uint32_t seconds)
{
    if (is_gateway()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (seconds == 0 || seconds > SLEEP_MAX_S) {
        return ESP_ERR_INVALID_ARG;
    }
    rtc_msg_t m = { .type = MSG_SLEEP, .mode = mode, .seconds = seconds };
    return xQueueSend(s_q, &m, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t rtc_mgr_sleep_until(rtc_sleep_mode_t mode, time_t when)
{
    time_t now;
    bool   valid;
    rtc_mgr_get_time(&now, &valid);
    if (!valid) {
        return ESP_ERR_INVALID_STATE;
    }
    if (when <= now) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Measured from the sleep, not from now: the grace delay comes first. */
    uint32_t grace_s = (RTC_SLEEP_GRACE_MS + 999) / 1000;
    uint32_t s = (uint32_t)(when - now);
    return rtc_mgr_sleep_for(mode, s > grace_s ? s - grace_s : 1);
}

static esp_err_t duty_apply(const rtc_duty_t *d)
{
    esp_err_t err = duty_save(d);
    if (err != ESP_OK) {
        return err;
    }
    portENTER_CRITICAL(&s_lock);
    s_duty = *d;
    portEXIT_CRITICAL(&s_lock);

    rtc_msg_t m = { .type = MSG_DUTY_RESTART };
    xQueueSend(s_q, &m, 0);
    return ESP_OK;
}

esp_err_t rtc_mgr_duty_set(const rtc_duty_t *duty)
{
    if (is_gateway()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (duty->awake_s < RTC_DUTY_MIN_AWAKE_S || duty->sleep_s == 0 ||
        duty->sleep_s > SLEEP_MAX_S) {
        return ESP_ERR_INVALID_ARG;
    }
    rtc_duty_t d = *duty;
    d.enabled = true;
    ESP_LOGI(TAG, "duty cycle: %s, awake %u s, asleep %u s%s", rtc_mgr_mode_name(d.mode),
             (unsigned)d.awake_s, (unsigned)d.sleep_s, d.align ? ", aligned" : "");
    return duty_apply(&d);
}

esp_err_t rtc_mgr_duty_stop(void)
{
    rtc_duty_t d;
    rtc_mgr_duty_get(&d);
    d.enabled = false;
    ESP_LOGI(TAG, "duty cycle off");
    return duty_apply(&d);
}

void rtc_mgr_duty_get(rtc_duty_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_duty;
    portEXIT_CRITICAL(&s_lock);
}

rtc_wake_cause_t rtc_mgr_wake_cause(void)
{
    return s_wake;
}

void rtc_mgr_status(rtc_status_t *out)
{
    rtc_mgr_get_time(&out->now, &out->valid);
    out->backend = s_dev->ops->backend;
    out->wake    = s_wake;
    rtc_mgr_duty_get(&out->duty);
}

esp_err_t rtc_mgr_apply(const rtc_req_t *req)
{
    switch (req->op) {
    case RTC_REQ_GET:
        return ESP_OK;
    case RTC_REQ_TIME_SET:
        return rtc_mgr_set_time((time_t)req->a);
    case RTC_REQ_SLEEP:
        return req->absolute ? rtc_mgr_sleep_until(req->mode, (time_t)req->a)
                             : rtc_mgr_sleep_for(req->mode, req->a);
    case RTC_REQ_DUTY: {
        rtc_duty_t d = {
            .enabled = true,
            .mode    = req->mode,
            .awake_s = req->a,
            .sleep_s = req->b,
            .align   = req->absolute,
        };
        return rtc_mgr_duty_set(&d);
    }
    case RTC_REQ_DUTY_OFF:
        return rtc_mgr_duty_stop();
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

/* --- Text helpers -------------------------------------------------------- */

const char *rtc_mgr_backend_name(rtc_backend_t b)
{
    return b == RTC_BACKEND_DS3231 ? "DS3231" : "INTERNAL";
}

const char *rtc_mgr_wake_name(rtc_wake_cause_t w)
{
    switch (w) {
    case RTC_WAKE_RESET: return "RESET";
    case RTC_WAKE_TIMER: return "TIMER";
    case RTC_WAKE_ALARM: return "ALARM";
    default:             return "OTHER";
    }
}

const char *rtc_mgr_mode_name(rtc_sleep_mode_t m)
{
    return m == RTC_SLEEP_DEEP ? "deep" : "light";
}

void rtc_mgr_format_iso(time_t t, char *out, size_t size)
{
    rtc_civil_t c;
    rtc_epoch_to_civil(t, &c);
    snprintf(out, size, "%04d-%02d-%02dT%02d:%02d:%02dZ",
             c.year, c.month, c.day, c.hour, c.min, c.sec);
}

void rtc_mgr_format_status(const rtc_status_t *st, char *out, size_t size)
{
    char iso[24];
    char duty[40];

    rtc_mgr_format_iso(st->now, iso, sizeof(iso));
    if (st->duty.enabled) {
        snprintf(duty, sizeof(duty), "%s/%u/%u%s", rtc_mgr_mode_name(st->duty.mode),
                 (unsigned)st->duty.awake_s, (unsigned)st->duty.sleep_s,
                 st->duty.align ? "/align" : "");
    } else {
        snprintf(duty, sizeof(duty), "off");
    }
    snprintf(out, size, "time=%s valid=%d backend=%s wake=%s duty=%s",
             iso, st->valid ? 1 : 0, rtc_mgr_backend_name(st->backend),
             rtc_mgr_wake_name(st->wake), duty);
}

uint8_t rtc_mgr_result_code(esp_err_t err)
{
    switch (err) {
    case ESP_OK:                return RTC_RESULT_OK;
    case ESP_ERR_INVALID_ARG:   return RTC_RESULT_BAD_ARG;
    case ESP_ERR_INVALID_STATE: return RTC_RESULT_REFUSED;
    default:                    return RTC_RESULT_FAILED;
    }
}

const char *rtc_mgr_result_name(uint8_t code)
{
    switch (code) {
    case RTC_RESULT_OK:      return "OK";
    case RTC_RESULT_BAD_ARG: return "BAD_ARG";
    case RTC_RESULT_REFUSED: return "REFUSED";
    default:                 return "FAILED";
    }
}

bool rtc_mgr_parse_mode(const char *text, rtc_sleep_mode_t *out)
{
    if (strcasecmp(text, "LIGHT") == 0) {
        *out = RTC_SLEEP_LIGHT;
        return true;
    }
    if (strcasecmp(text, "DEEP") == 0) {
        *out = RTC_SLEEP_DEEP;
        return true;
    }
    return false;
}

bool rtc_mgr_parse_time(const char *text, time_t now, time_t *out)
{
    bool digits = *text != '\0';
    for (const char *p = text; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            digits = false;
            break;
        }
    }
    if (digits) {
        char         *end;
        unsigned long v = strtoul(text, &end, 10);
        *out = (time_t)v;
        return *end == '\0';
    }

    rtc_civil_t c = {0};
    char        tail[4] = "";

    if (strchr(text, '-') != NULL) {
        int n = sscanf(text, "%d-%d-%d%*1[T ]%d:%d:%d%3s",
                       &c.year, &c.month, &c.day, &c.hour, &c.min, &c.sec, tail);
        if (n < 5) {
            return false;
        }
        if (n < 6) {
            c.sec = 0;
        }
    } else {
        int n = sscanf(text, "%d:%d:%d%3s", &c.hour, &c.min, &c.sec, tail);
        if (n < 2) {
            return false;
        }
        if (n < 3) {
            c.sec = 0;
        }
        /* Time of day: today's date, pushed to tomorrow if already past. */
        rtc_civil_t today;
        rtc_epoch_to_civil(now, &today);
        c.year = today.year;
        c.month = today.month;
        c.day = today.day;
    }

    if (tail[0] != '\0' && strcasecmp(tail, "Z") != 0) {
        return false;
    }
    if (c.month < 1 || c.month > 12 || c.day < 1 || c.day > 31 ||
        c.hour < 0 || c.hour > 23 || c.min < 0 || c.min > 59 ||
        c.sec < 0 || c.sec > 59) {
        return false;
    }

    time_t t = rtc_civil_to_epoch(&c);
    if (strchr(text, '-') == NULL && t <= now) {
        t += 86400;
    }
    *out = t;
    return true;
}
