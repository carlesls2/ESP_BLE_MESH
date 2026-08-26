/* device_mode.h - Runtime NODE / GATEWAY personality
 *
 * The firmware ships both mesh personalities and picks one at runtime:
 *
 *   DEVICE_MODE_NODE     provisionable Generic OnOff server that broadcasts to
 *                        the group address at randomised intervals.
 *   DEVICE_MODE_GATEWAY  BLE Mesh Provisioner (provisions and configures other
 *                        nodes) plus a mesh <-> host bridge.
 *
 * A device boots as NODE and is promoted when a command arrives over UART or
 * SPI. The switch is applied live -- no reboot -- and persisted in NVS so the
 * role survives a power cycle.
 *
 * This module deliberately knows nothing about BLE Mesh. The mesh layer hands
 * it an apply callback via device_mode_register_apply_cb(), which keeps
 * lib/mode free of a circular dependency on lib/BLE.
 */

#ifndef _DEVICE_MODE_H_
#define _DEVICE_MODE_H_

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DEVICE_MODE_NODE    = 0,
    DEVICE_MODE_GATEWAY = 1,
} device_mode_t;

/* Applies the role to whatever subsystem registered it. Returning an error
 * makes device_mode_set() fail without persisting, so a mode the mesh stack
 * could not enter is never written to NVS. */
typedef esp_err_t (*device_mode_apply_cb_t)(device_mode_t mode);

/* Loads the stored mode from NVS (defaults to NODE) and drives the indicator
 * LED. Does not invoke the apply callback -- the mesh stack is not up yet at
 * this point; call device_mode_apply_stored() once it is.
 *
 * nvs_flash_init() must have been called first. */
esp_err_t device_mode_init(void);

device_mode_t device_mode_get(void);

/* Applies the mode live and, if the apply callback succeeded, persists it.
 * Safe to call from any task. */
esp_err_t device_mode_set(device_mode_t mode);

/* Re-applies the mode loaded by device_mode_init(). Call once the subsystem
 * that registered the apply callback is ready to act on it. */
esp_err_t device_mode_apply_stored(void);

const char *device_mode_name(device_mode_t mode);

void device_mode_register_apply_cb(device_mode_apply_cb_t cb);

#ifdef __cplusplus
}
#endif
#endif /* _DEVICE_MODE_H_ */
