/* rtc_dev.h - Low-level real-time clock device
 *
 * One interface, several chips. A backend fills in an ops table; everything
 * above (rtc_mgr) talks to the rtc_dev_t handle and never to a chip directly,
 * so adding a PCF8563 later is one new .c file and no change upstairs.
 *
 *   rtc_internal.c   the ESP32's own RTC timer. Always present, keeps counting
 *                    through deep sleep, loses the date on power-off, and has
 *                    no alarm output -- sleep wake-up uses the sleep timer.
 *   rtc_ds3231.c     DS3231 over I2C. Battery-backed, +-2 ppm, and its alarm
 *                    pulls INT/SQW low, which can wake the ESP32 from deep
 *                    sleep through an RTC-capable GPIO.
 *
 * Time is always UTC seconds since the Unix epoch. Time zones are the host's
 * business.
 */

#ifndef _RTC_DEV_H_
#define _RTC_DEV_H_

#include <stdbool.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RTC_BACKEND_INTERNAL = 0,
    RTC_BACKEND_DS3231   = 1,
} rtc_backend_t;

typedef struct rtc_dev rtc_dev_t;

typedef struct {
    rtc_backend_t backend;
    const char   *name;

    esp_err_t (*get_time)(rtc_dev_t *dev, time_t *out);
    esp_err_t (*set_time)(rtc_dev_t *dev, time_t t);

    /* False when the clock cannot be trusted: never set, or (for a chip with
     * a backup battery) the oscillator stopped since the last set. */
    bool      (*time_valid)(rtc_dev_t *dev);

    /* Hardware alarm that drives alarm_gpio low at `when`. Backends without
     * one return ESP_ERR_NOT_SUPPORTED; the middleware then relies on the
     * ESP32 sleep timer alone. */
    esp_err_t (*set_alarm)(rtc_dev_t *dev, time_t when);
    /* Disarms the alarm and releases the interrupt line. */
    esp_err_t (*clear_alarm)(rtc_dev_t *dev);
} rtc_dev_ops_t;

struct rtc_dev {
    const rtc_dev_ops_t *ops;
    int                  alarm_gpio;  /* RTC-capable GPIO wired to the alarm, or -1 */
    void                *ctx;         /* backend private state */
};

/* The internal RTC. Never fails; the returned device is a static. */
rtc_dev_t *rtc_internal_get(void);

/* Thin wrappers so callers never touch the ops table. */
static inline esp_err_t rtc_dev_get_time(rtc_dev_t *d, time_t *out) { return d->ops->get_time(d, out); }
static inline esp_err_t rtc_dev_set_time(rtc_dev_t *d, time_t t)    { return d->ops->set_time(d, t); }
static inline bool      rtc_dev_time_valid(rtc_dev_t *d)            { return d->ops->time_valid(d); }
static inline esp_err_t rtc_dev_set_alarm(rtc_dev_t *d, time_t t)   { return d->ops->set_alarm(d, t); }
static inline esp_err_t rtc_dev_clear_alarm(rtc_dev_t *d)           { return d->ops->clear_alarm(d); }
static inline bool      rtc_dev_has_alarm(const rtc_dev_t *d)       { return d->alarm_gpio >= 0; }

/* --- Calendar helpers (UTC, no libc time zone involvement) --------------- */

typedef struct {
    int year;   /* e.g. 2026 */
    int month;  /* 1-12 */
    int day;    /* 1-31 */
    int hour, min, sec;
} rtc_civil_t;

time_t rtc_civil_to_epoch(const rtc_civil_t *c);
void   rtc_epoch_to_civil(time_t t, rtc_civil_t *out);

#ifdef __cplusplus
}
#endif
#endif /* _RTC_DEV_H_ */
