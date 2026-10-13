/* rtc_internal.c - The ESP32's own RTC as an rtc_dev_t
 *
 * ESP-IDF already keeps the system clock running across deep sleep (it is
 * derived from the RTC timer, whose boot offset lives in RTC memory), so this
 * backend is just gettimeofday/settimeofday plus a validity flag.
 *
 * Accuracy is the weak point: CONFIG_ESP32_RTC_CLK_SRC_INT_RC is a ~150 kHz RC
 * oscillator good to about 5 %, so a long deep sleep can wake minutes early or
 * late. Fit a 32 kHz crystal or a DS3231 when that matters.
 */

#include "rtc_dev.h"

#include <sys/time.h>

#include "esp_attr.h"

/* RTC slow memory: survives deep sleep, reset to zero on power-on. That is
 * exactly the lifetime of the internal clock's own value. */
static RTC_DATA_ATTR bool s_valid;

static esp_err_t internal_get_time(rtc_dev_t *dev, time_t *out)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    *out = tv.tv_sec;
    return ESP_OK;
}

static esp_err_t internal_set_time(rtc_dev_t *dev, time_t t)
{
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) {
        return ESP_FAIL;
    }
    s_valid = true;
    return ESP_OK;
}

static bool internal_time_valid(rtc_dev_t *dev)
{
    return s_valid;
}

static esp_err_t internal_set_alarm(rtc_dev_t *dev, time_t when)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t internal_clear_alarm(rtc_dev_t *dev)
{
    return ESP_OK;
}

static const rtc_dev_ops_t s_ops = {
    .backend     = RTC_BACKEND_INTERNAL,
    .name        = "INTERNAL",
    .get_time    = internal_get_time,
    .set_time    = internal_set_time,
    .time_valid  = internal_time_valid,
    .set_alarm   = internal_set_alarm,
    .clear_alarm = internal_clear_alarm,
};

static rtc_dev_t s_dev = {
    .ops        = &s_ops,
    .alarm_gpio = -1,
    .ctx        = NULL,
};

rtc_dev_t *rtc_internal_get(void)
{
    return &s_dev;
}

/* --- Calendar helpers ----------------------------------------------------
 *
 * Howard Hinnant's days-from-civil algorithm: exact for the proleptic
 * Gregorian calendar, no tables, no libc time zone state. newlib has no
 * timegm(), and mktime() would apply TZ. */

time_t rtc_civil_to_epoch(const rtc_civil_t *c)
{
    int      y   = c->year - (c->month <= 2);
    int      era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned mp  = (unsigned)((c->month + 9) % 12);
    unsigned doy = (153 * mp + 2) / 5 + (unsigned)c->day - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long     days = (long)era * 146097 + (long)doe - 719468;

    return (time_t)days * 86400 + c->hour * 3600 + c->min * 60 + c->sec;
}

void rtc_epoch_to_civil(time_t t, rtc_civil_t *out)
{
    long days = (long)(t / 86400);
    long secs = (long)(t % 86400);
    if (secs < 0) {
        secs += 86400;
        days--;
    }

    days += 719468;
    long     era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned doe = (unsigned)(days - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp  = (5 * doy + 2) / 153;

    out->day   = (int)(doy - (153 * mp + 2) / 5 + 1);
    out->month = (int)(mp < 10 ? mp + 3 : mp - 9);
    out->year  = (int)(yoe + era * 400 + (out->month <= 2));
    out->hour  = (int)(secs / 3600);
    out->min   = (int)((secs % 3600) / 60);
    out->sec   = (int)(secs % 60);
}
