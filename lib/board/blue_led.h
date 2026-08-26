#ifndef _BLUE_LED_H_
#define _BLUE_LED_H_

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"

#ifndef BLUE_LED_GPIO
#define BLUE_LED_GPIO GPIO_NUM_2
#endif

void blue_led_init(void);
void blue_led_on(void);
void blue_led_off(void);
void blue_led_set(bool on);
void blue_led_toggle(void);
bool blue_led_is_on(void);

/* Non-blocking blink: schedules `times` on/off cycles using esp_timer.
 * Returns immediately. Pass times = 0 for infinite blink until
 * blue_led_blink_stop() is called. */
void blue_led_blink(uint32_t times, uint32_t period_ms);
void blue_led_blink_stop(void);
bool blue_led_blink_active(void);

#endif
