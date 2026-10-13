/* rtc_ds3231.h - DS3231 backend for rtc_dev_t
 *
 * Wiring (3.3 V module; most carry their own pull-ups on SDA/SCL/INT):
 *
 *     DS3231    ESP32
 *     SDA   ->  sda_gpio        (GPIO21 by convention)
 *     SCL   ->  scl_gpio        (GPIO22 by convention)
 *     INT   ->  alarm_gpio      must be RTC-capable to wake from deep sleep:
 *                               0, 2, 4, 12-15, 25-27, 32-39 (33 suggested)
 *     VCC/GND
 *
 * The alarm uses Alarm 1 matched on date, hour, minute and second, with INTCN
 * set so INT/SQW goes low on a match and stays low until cleared.
 */

#ifndef _RTC_DS3231_H_
#define _RTC_DS3231_H_

#include "driver/i2c.h"
#include "rtc_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    i2c_port_t port;
    int        sda_gpio;
    int        scl_gpio;
    int        alarm_gpio;   /* -1 when INT is not wired */
    uint32_t   clk_hz;       /* 100000 is safe for any module */
} rtc_ds3231_cfg_t;

/* Installs the I2C driver and probes the chip. ESP_ERR_NOT_FOUND when nothing
 * answers at 0x68, in which case the driver is removed again and the caller
 * should fall back to the internal RTC. */
esp_err_t rtc_ds3231_create(const rtc_ds3231_cfg_t *cfg, rtc_dev_t **out);

#ifdef __cplusplus
}
#endif
#endif /* _RTC_DS3231_H_ */
