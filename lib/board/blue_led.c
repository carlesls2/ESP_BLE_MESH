#include "blue_led.h"

#include "esp_log.h"
#include "esp_timer.h"

#define TAG "BLUE_LED"

static bool s_initialized = false;
static bool s_state = false;

static esp_timer_handle_t s_blink_timer = NULL;
static uint32_t s_blink_half_us = 0;
static uint32_t s_blink_edges_left = 0;   /* 0 = infinite */
static bool s_blink_infinite = false;

static void blink_timer_cb(void *arg);

void blue_led_init(void)
{
    if (s_initialized) {
        return;
    }
    gpio_reset_pin(BLUE_LED_GPIO);
    gpio_set_direction(BLUE_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BLUE_LED_GPIO, 0);
    s_state = false;

    const esp_timer_create_args_t args = {
        .callback = blink_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "blue_led_blink",
    };
    esp_timer_create(&args, &s_blink_timer);

    s_initialized = true;
    ESP_LOGI(TAG, "blue LED initialized on GPIO %d", BLUE_LED_GPIO);
}

void blue_led_set(bool on)
{
    if (!s_initialized) {
        blue_led_init();
    }
    gpio_set_level(BLUE_LED_GPIO, on ? 1 : 0);
    s_state = on;
}

void blue_led_on(void)   { blue_led_set(true);  }
void blue_led_off(void)  { blue_led_set(false); }
void blue_led_toggle(void) { blue_led_set(!s_state); }
bool blue_led_is_on(void)  { return s_state; }

static void blink_timer_cb(void *arg)
{
    (void)arg;
    gpio_set_level(BLUE_LED_GPIO, s_state ? 0 : 1);
    s_state = !s_state;

    if (!s_blink_infinite) {
        if (s_blink_edges_left > 0) {
            s_blink_edges_left--;
        }
        if (s_blink_edges_left == 0) {
            return; /* one-shot timer stops naturally */
        }
    }
    esp_timer_start_once(s_blink_timer, s_blink_half_us);
}

void blue_led_blink(uint32_t times, uint32_t period_ms)
{
    if (!s_initialized) {
        blue_led_init();
    }
    if (period_ms == 0) {
        period_ms = 500;
    }

    esp_timer_stop(s_blink_timer);

    s_blink_half_us = (period_ms / 2) * 1000U;
    s_blink_infinite = (times == 0);
    s_blink_edges_left = s_blink_infinite ? 0 : (times * 2);

    /* Kick off the first edge immediately via the timer task. */
    esp_timer_start_once(s_blink_timer, 0);
}

void blue_led_blink_stop(void)
{
    if (s_blink_timer) {
        esp_timer_stop(s_blink_timer);
    }
    s_blink_edges_left = 0;
    s_blink_infinite = false;
}

bool blue_led_blink_active(void)
{
    return s_blink_infinite || s_blink_edges_left > 0;
}
