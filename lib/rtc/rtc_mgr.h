/* rtc_mgr.h - Time keeping and sleep scheduling
 *
 * The middleware between the RTC device (rtc_dev.h) and everything that wants
 * time or sleep: host commands, the mesh, application code.
 *
 *   TIME     one clock for the whole firmware. The system clock is the working
 *            copy; a battery-backed chip, when fitted, is the master that sets
 *            it at boot and is written on every set.
 *
 *   SLEEP    light:  RAM kept, execution resumes after the call site. ~0.8 mA.
 *            deep:   RAM lost, the chip reboots on wake. ~10 uA.
 *            Either wakes on the sleep timer, and on the external RTC's alarm
 *            pin when one is wired -- the timer then acts as a backstop.
 *
 *   DUTY     a repeating "awake A s, asleep S s" schedule, optionally aligned
 *            to wall-clock multiples of A+S (e.g. wake on every 10-minute
 *            mark). Stored in NVS, so a deep-sleeping node resumes it on every
 *            wake with no host involved.
 *
 * Sleep is always carried out by the middleware's own task, a short grace
 * period after it is requested, so whoever asked -- a UART command, a mesh
 * message -- gets its reply out before the radio goes quiet.
 *
 * A GATEWAY never sleeps: it is the mesh's provisioner, the host's only way
 * in, and (from NVS boot) cannot send until its role is toggled. Requests are
 * refused with ESP_ERR_INVALID_STATE while the board is in that role.
 *
 * Neither mode keeps the mesh alive: the radio is off while asleep, so the
 * board cannot relay, answer, or hear HELLOs until it wakes.
 */

#ifndef _RTC_MGR_H_
#define _RTC_MGR_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"
#include "rtc_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RTC_SLEEP_LIGHT = 0,
    RTC_SLEEP_DEEP  = 1,
} rtc_sleep_mode_t;

typedef enum {
    RTC_WAKE_RESET = 0,   /* power-on or any reset -- not a sleep wake */
    RTC_WAKE_TIMER = 1,
    RTC_WAKE_ALARM = 2,   /* the external RTC's alarm pin */
    RTC_WAKE_OTHER = 3,
} rtc_wake_cause_t;

/* Shortest awake window a duty cycle may have. Long enough for a deep-slept
 * node to reboot, rejoin the mesh and hear a command. */
#define RTC_DUTY_MIN_AWAKE_S 5

/* Delay between a sleep request and the sleep itself. */
#define RTC_SLEEP_GRACE_MS 1000

typedef struct {
    bool             enabled;
    rtc_sleep_mode_t mode;
    uint32_t         awake_s;
    uint32_t         sleep_s;
    bool             align;    /* wake on wall-clock multiples of awake+sleep */
} rtc_duty_t;

typedef struct {
    time_t           now;
    bool             valid;
    rtc_backend_t    backend;
    rtc_wake_cause_t wake;
    rtc_duty_t       duty;
} rtc_status_t;

/* One request shape for every caller -- the local command channel and the mesh
 * both build one of these and hand it to rtc_mgr_apply(). */
typedef enum {
    RTC_REQ_GET      = 0,
    RTC_REQ_TIME_SET = 1,   /* a = epoch */
    RTC_REQ_SLEEP    = 2,   /* a = seconds, or an epoch when `absolute` */
    RTC_REQ_DUTY     = 3,   /* a = awake_s, b = sleep_s, `absolute` = align */
    RTC_REQ_DUTY_OFF = 4,
} rtc_req_op_t;

typedef struct {
    rtc_req_op_t     op;
    rtc_sleep_mode_t mode;
    bool             absolute;
    uint32_t         a;
    uint32_t         b;
} rtc_req_t;

/* Picks the backend (DS3231 when configured and present, else internal),
 * syncs the system clock from it, records why we woke, and starts the task
 * that runs sleeps and the stored duty cycle. Call after nvs_flash_init() and
 * device_mode_init(). */
esp_err_t rtc_mgr_init(void);

/* --- Time ---------------------------------------------------------------- */

esp_err_t rtc_mgr_get_time(time_t *now, bool *valid);
esp_err_t rtc_mgr_set_time(time_t t);

/* --- Sleep --------------------------------------------------------------- */

/* Both return once the request is queued; the sleep follows after
 * RTC_SLEEP_GRACE_MS. sleep_until needs a valid clock. */
esp_err_t rtc_mgr_sleep_for(rtc_sleep_mode_t mode, uint32_t seconds);
esp_err_t rtc_mgr_sleep_until(rtc_sleep_mode_t mode, time_t when);

/* --- Duty cycle ---------------------------------------------------------- */

esp_err_t rtc_mgr_duty_set(const rtc_duty_t *duty);   /* starts a fresh awake window */
esp_err_t rtc_mgr_duty_stop(void);
void      rtc_mgr_duty_get(rtc_duty_t *out);

/* --- Status -------------------------------------------------------------- */

rtc_wake_cause_t rtc_mgr_wake_cause(void);
void             rtc_mgr_status(rtc_status_t *out);

/* Executes a request. For RTC_REQ_GET it only validates. */
esp_err_t rtc_mgr_apply(const rtc_req_t *req);

/* --- Text helpers, shared by the command channel and the gateway bridge -- */

const char *rtc_mgr_backend_name(rtc_backend_t b);
const char *rtc_mgr_wake_name(rtc_wake_cause_t w);
const char *rtc_mgr_mode_name(rtc_sleep_mode_t m);

/* "2026-09-30T14:05:00Z" */
void rtc_mgr_format_iso(time_t t, char *out, size_t size);

/* "time=<iso> valid=1 backend=DS3231 wake=TIMER duty=deep/10/300/align" */
void rtc_mgr_format_status(const rtc_status_t *st, char *out, size_t size);

/* A request's outcome as one byte, for the mesh wire and the host line:
 * OK, BAD_ARG (out of range), REFUSED (gateway, or clock not set), FAILED. */
#define RTC_RESULT_OK      0
#define RTC_RESULT_BAD_ARG 1
#define RTC_RESULT_REFUSED 2
#define RTC_RESULT_FAILED  3

uint8_t     rtc_mgr_result_code(esp_err_t err);
const char *rtc_mgr_result_name(uint8_t code);

/* Parses "LIGHT"/"DEEP" (case-insensitive). */
bool rtc_mgr_parse_mode(const char *text, rtc_sleep_mode_t *out);

/* Accepts an epoch ("1790000000"), an ISO date-time ("2026-09-30T14:05:00",
 * trailing Z optional, 'T' or ' '), or a time of day ("14:05" / "14:05:30"),
 * which means its next occurrence after `now` in UTC. */
bool rtc_mgr_parse_time(const char *text, time_t now, time_t *out);

#ifdef __cplusplus
}
#endif
#endif /* _RTC_MGR_H_ */
