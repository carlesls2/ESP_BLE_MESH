/* neighbor.c - Nodes this board hears directly over the radio */

#include "neighbor.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define TAG "NEIGH"

/* Mirrors ESP_BLE_MESH_ADDR_IS_UNICAST without pulling in the mesh headers. */
#define ADDR_IS_UNICAST(a) ((a) != 0x0000 && (a) < 0x8000)

typedef struct {
    uint16_t addr;          /* 0x0000 = free slot */
    int8_t   rssi_last;
    int16_t  rssi_avg_x16;  /* average scaled by 16, so no float in the hot path */
    uint16_t heard;
    int64_t  last_us;
} slot_t;

static slot_t      s_table[NEIGH_MAX];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

void neighbor_note(uint16_t src, uint16_t own_addr, uint8_t recv_ttl, int8_t rssi)
{
    if (!ADDR_IS_UNICAST(src) || src == own_addr) {
        return;
    }
    if (recv_ttl != 0 && recv_ttl != NEIGH_SENDER_TTL) {
        ESP_LOGD(TAG, "0x%04x relayed (ttl %u), not a neighbor", src, recv_ttl);
        return;
    }

    int64_t now = esp_timer_get_time();

    portENTER_CRITICAL(&s_lock);

    slot_t *hit = NULL;
    slot_t *victim = &s_table[0];
    for (size_t i = 0; i < NEIGH_MAX; i++) {
        slot_t *s = &s_table[i];
        if (s->addr == src) {
            hit = s;
            break;
        }
        /* Prefer a free slot, then the one silent the longest. */
        if (victim->addr != 0x0000 &&
            (s->addr == 0x0000 || s->last_us < victim->last_us)) {
            victim = s;
        }
    }

    if (hit == NULL) {
        hit = victim;
        hit->addr = src;
        hit->rssi_avg_x16 = (int16_t)(rssi * 16);
        hit->heard = 0;
    } else {
        /* avg += (sample - avg) / 8 */
        hit->rssi_avg_x16 += (int16_t)((rssi * 16 - hit->rssi_avg_x16) / 8);
    }
    hit->rssi_last = rssi;
    hit->last_us = now;
    if (hit->heard < UINT16_MAX) {
        hit->heard++;
    }

    portEXIT_CRITICAL(&s_lock);
}

size_t neighbor_snapshot(neighbor_entry_t *out, size_t max)
{
    slot_t copy[NEIGH_MAX];

    portENTER_CRITICAL(&s_lock);
    memcpy(copy, s_table, sizeof(copy));
    portEXIT_CRITICAL(&s_lock);

    int64_t now = esp_timer_get_time();
    size_t  n = 0;

    for (size_t i = 0; i < NEIGH_MAX && n < max; i++) {
        const slot_t *s = &copy[i];
        if (s->addr == 0x0000) {
            continue;
        }
        int64_t age = (now - s->last_us) / 1000000;
        if (age > NEIGH_STALE_S) {
            continue;
        }

        neighbor_entry_t e = {
            .addr      = s->addr,
            .rssi_last = s->rssi_last,
            .rssi_avg  = (int8_t)(s->rssi_avg_x16 / 16),
            .age_s     = (uint16_t)age,
            .heard     = s->heard,
        };

        /* Insertion sort, strongest first: at most 16 entries. */
        size_t j = n;
        while (j > 0 && out[j - 1].rssi_avg < e.rssi_avg) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = e;
        n++;
    }
    return n;
}

void neighbor_clear(void)
{
    portENTER_CRITICAL(&s_lock);
    memset(s_table, 0, sizeof(s_table));
    portEXIT_CRITICAL(&s_lock);
}
