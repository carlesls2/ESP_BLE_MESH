/* telemetry.h - Live device vitals
 *
 * Supplies the dynamic rows of the attribute registry (uptime, heap, battery,
 * RSSI, role, unicast address) and the same values as one packed snapshot for
 * the mesh TELEM message.
 *
 * Two ways out, one source of truth:
 *
 *   dev_attr providers  ->  ASK <addr> UPTIME HEAP   per-attribute, uniform
 *                           with the rest of the identity commands, TLV-encoded
 *   telemetry_snapshot  ->  TELEM <addr>             everything at once, packed
 *
 * The per-attribute path costs 2 header bytes per value and segments badly once
 * you ask for more than a couple; the snapshot fits the lot in 21 bytes. Both
 * read the same functions below, so they can never disagree.
 *
 * This module knows nothing about the mesh wire format -- mesh_attr_model.c
 * maps the snapshot onto the packet. It does read the stack's own primary
 * element address, which is an ESP-IDF API rather than a lib/BLE dependency.
 */

#ifndef _TELEMETRY_H_
#define _TELEMETRY_H_

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t uptime_s;
    uint32_t free_heap;
    uint32_t min_free_heap;
    uint16_t batt_mv;
    uint8_t  batt_pct;
    bool     batt_valid;    /* false when no divider is fitted */
    int8_t   rssi;          /* of the last mesh message received */
    bool     rssi_valid;    /* false until something has been heard */
    uint8_t  reset_reason;  /* esp_reset_reason_t */
    uint16_t unicast;       /* 0x0000 while unprovisioned */
    bool     is_gateway;
} telemetry_snapshot_t;

/* Brings up the battery ADC and registers every dynamic attribute provider.
 * Call before dev_identity_init() so the boot log can print them; calling it
 * later still works, the values simply stay quiet until then. */
esp_err_t telemetry_init(void);

/* Records the RSSI of a received mesh message. Called from the vendor model's
 * receive path, which is the only place that sees esp_ble_mesh_msg_ctx_t. */
void telemetry_note_rssi(int8_t rssi);

/* Fills every field. Never fails -- unavailable values are flagged by their
 * companion *_valid member rather than by a return code. */
void telemetry_snapshot(telemetry_snapshot_t *out);

/* Short name for an esp_reset_reason_t, e.g. "POWERON", "BROWNOUT". Used both
 * for this device's own attribute and by the gateway when rendering a remote
 * node's TELEM reply, so the two always read the same. */
const char *telemetry_reset_reason_str(uint8_t reason);

#ifdef __cplusplus
}
#endif
#endif /* _TELEMETRY_H_ */
