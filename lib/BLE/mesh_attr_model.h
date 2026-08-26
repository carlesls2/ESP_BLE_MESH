/* mesh_attr_model.h - Vendor model carrying device attributes over the mesh
 *
 * Layer 3 of the identity stack. Turns the local attribute store into something
 * the gateway can query and write across the network.
 *
 * Every board compiles in both halves, matching how the NODE/GATEWAY split
 * already carries both mesh roles:
 *
 *   server  answers ATTR_GET, applies ATTR_SET   -- what a slave uses
 *   client  sends ATTR_GET / ATTR_SET            -- what a gateway uses
 *
 * WIRE SIZE. A mesh access message carries 11 bytes unsegmented, and the 3-byte
 * vendor opcode leaves 8. A reply holding every attribute is ~88 bytes and so
 * segments into 8 blocks -- correct, but slow and collision-prone. ATTR_GET
 * therefore carries a list of requested ids so a narrow query stays in one
 * message, and replies to a group-addressed request are staggered.
 */

#ifndef _MESH_ATTR_MODEL_H_
#define _MESH_ATTR_MODEL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_ble_mesh_defs.h"
#include "esp_err.h"

#include "dev_attr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Vendor model ids under CID_ESP. */
#define MESH_ATTR_MODEL_ID_SERVER 0x0000
#define MESH_ATTR_MODEL_ID_CLIENT 0x0001

/* Opcode low bytes; the full 3-byte opcodes are built in the .c with
 * ESP_BLE_MESH_MODEL_OP_3(b0, CID_ESP). */
#define MESH_ATTR_OP_B0_GET        0x10
#define MESH_ATTR_OP_B0_STATUS     0x11
#define MESH_ATTR_OP_B0_SET        0x12
#define MESH_ATTR_OP_B0_SET_STATUS 0x13

/* Result codes carried by ATTR_SET_STATUS. */
#define MESH_ATTR_RESULT_OK          0x00
#define MESH_ATTR_RESULT_UNKNOWN     0x01  /* no such attribute */
#define MESH_ATTR_RESULT_READ_ONLY   0x02  /* not remotely writable */
#define MESH_ATTR_RESULT_BAD_VALUE   0x03  /* wrong length or unparseable */

/* Delivered to the application when a node answers. `text` is a pre-rendered
 * one-line summary suitable for handing straight to the host bridge. */
typedef void (*mesh_attr_status_cb_t)(uint16_t src_addr, const char *text);
typedef void (*mesh_attr_set_status_cb_t)(uint16_t src_addr, uint8_t result);

/* The vendor models, to drop into an element's vendor slot:
 *
 *     ESP_BLE_MESH_ELEMENT(0, root_models, mesh_attr_vnd_models)
 *
 * Exposed as an array rather than behind a getter because ESP_BLE_MESH_ELEMENT
 * expands ARRAY_SIZE over it, which a pointer cannot satisfy. */
#define MESH_ATTR_VND_MODEL_COUNT 2
extern esp_ble_mesh_model_t mesh_attr_vnd_models[MESH_ATTR_VND_MODEL_COUNT];

/* Registers the custom-model callback and the reply stagger timer. Call after
 * esp_ble_mesh_init() has succeeded. */
esp_err_t mesh_attr_model_init(void);

void mesh_attr_model_register_cbs(mesh_attr_status_cb_t status_cb,
                                  mesh_attr_set_status_cb_t set_status_cb);

/* --- Gateway side -------------------------------------------------------- */

/* Queries `dst` for the listed attributes; pass ids == NULL / id_count == 0 for
 * all of them. `dst` may be a unicast address or a group address -- a group
 * request reaches every subscriber and each replies unicast after its own
 * random delay. */
esp_err_t mesh_attr_get(uint16_t dst, const dev_attr_id_t *ids, size_t id_count);

/* Writes one attribute on `dst`. Refused at the far end unless the attribute
 * carries DEV_ATTR_FLAG_REMOTE. */
esp_err_t mesh_attr_set(uint16_t dst, dev_attr_id_t id, const char *text);

/* --- Group membership ---------------------------------------------------- */

/* Subscribes the attribute server model to whatever GROUP_ADDR currently holds,
 * dropping the previous subscription first. Call once the node is provisioned
 * (a subscription needs an element address), and again whenever GROUP_ADDR
 * changes -- wiring it to dev_identity_register_change_cb() does both.
 *
 * A GROUP_ADDR of 0x0000 means unassigned and only unsubscribes. */
esp_err_t mesh_attr_apply_group(void);

#ifdef __cplusplus
}
#endif
#endif /* _MESH_ATTR_MODEL_H_ */
