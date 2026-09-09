/* batt_adc.h - Battery voltage over an ADC1 divider
 *
 * A battery sits above the ESP32's 3.3 V input range, so it is measured through
 * a resistor divider and scaled back up here.
 *
 * CONFIGURATION, from platformio.ini build_flags:
 *
 *   -DBATT_ADC_GPIO=34        which pin the divider feeds
 *   -DBATT_DIVIDER_NUM=2      \  vbatt_mv = measured_mv * NUM / DEN
 *   -DBATT_DIVIDER_DEN=1      /  two equal resistors halve the voltage -> 2/1
 *
 * Leave BATT_ADC_GPIO undefined on a board with no divider fitted: every read
 * then reports unavailable, and the attribute is omitted from replies instead
 * of being reported as a flat battery.
 *
 * ONLY ADC1 PINS WORK: GPIO 32-39. ADC2 is claimed by the radio and reads fail
 * while the mesh is up. 34-39 are input-only, so they cannot be driven by a
 * stray pinMode -- prefer them.
 *
 * This targets ESP-IDF 4.4, which predates the esp_adc/adc_oneshot.h driver;
 * the legacy driver/adc.h + esp_adc_cal.h pair is the correct API here.
 */

#ifndef _BATT_ADC_H_
#define _BATT_ADC_H_

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Configures the ADC and loads the factory calibration. Safe to call when no
 * pin is configured -- it succeeds and every later read reports unavailable. */
esp_err_t batt_adc_init(void);

/* True if a divider is configured and the reading succeeded. `mv` is the
 * battery voltage in millivolts, already scaled back through the divider. */
bool batt_adc_read_mv(uint16_t *mv);

/* State of charge, 0-100, from a single-cell LiPo discharge curve. False when
 * no reading is available. */
bool batt_adc_read_pct(uint8_t *pct);

/* True if a divider pin is configured at all. */
bool batt_adc_present(void);

#ifdef __cplusplus
}
#endif
#endif /* _BATT_ADC_H_ */
