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
#define MESH_ATTR_OP_B0_GET          0x10
#define MESH_ATTR_OP_B0_STATUS       0x11
#define MESH_ATTR_OP_B0_SET          0x12
#define MESH_ATTR_OP_B0_SET_STATUS   0x13
#define MESH_ATTR_OP_B0_MSG          0x14  /* free text, gateway -> node(s) */
#define MESH_ATTR_OP_B0_TELEM_GET    0x15  /* no payload */
#define MESH_ATTR_OP_B0_TELEM_STATUS 0x16  /* one mesh_telem_t */

/* --- Telemetry fast path -------------------------------------------------
 *
 * Asking for every vital as TLV attributes costs 2 header bytes per value and
 * runs to ~155 bytes, which segments into 13 blocks. The same data packed into
 * the struct below is 21 bytes -- still segmented, but into 2. That is the
 * whole reason this opcode pair exists next to ATTR_GET; use TELEM for the
 * routine "how is everyone doing" sweep and ATTR_GET when you want a named
 * subset or the stored identity fields alongside.
 *
 * Little-endian on the wire, matching the TLV convention and the CPU, so the
 * struct is memcpy'd rather than field-by-field encoded. `ver` leads so a
 * gateway meeting an older or newer node can say so instead of misparsing. */
#define MESH_TELEM_VER 1

/* Flags byte. */
#define MESH_TELEM_FLAG_GATEWAY    (1u << 0)
#define MESH_TELEM_FLAG_BATT_VALID (1u << 1)
#define MESH_TELEM_FLAG_RSSI_VALID (1u << 2)

typedef struct __attribute__((packed)) {
    uint8_t  ver;           /* MESH_TELEM_VER */
    uint16_t unicast;       /* 0x0000 while unprovisioned */
    uint32_t uptime_s;
    uint32_t free_heap;
    uint32_t min_free_heap;
    uint16_t batt_mv;
    uint8_t  batt_pct;
    int8_t   rssi;          /* of the last mesh message the node heard */
    uint8_t  reset_reason;  /* esp_reset_reason_t */
    uint8_t  flags;         /* MESH_TELEM_FLAG_* */
} mesh_telem_t;

/* Longest text a MSG may carry. Segments on the air past 8 bytes, same as a
 * full attribute reply; the bound exists so the receive side can use a fixed
 * stack buffer. */
#define MESH_ATTR_MSG_MAX_LEN 80

/* Result codes carried by ATTR_SET_STATUS. */
#define MESH_ATTR_RESULT_OK          0x00
#define MESH_ATTR_RESULT_UNKNOWN     0x01  /* no such attribute */
#define MESH_ATTR_RESULT_READ_ONLY   0x02  /* not remotely writable */
#define MESH_ATTR_RESULT_BAD_VALUE   0x03  /* wrong length or unparseable */

/* Delivered to the application when a node answers. `text` is a pre-rendered
 * one-line summary suitable for handing straight to the host bridge. */
typedef void (*mesh_attr_status_cb_t)(uint16_t src_addr, const char *text);
typedef void (*mesh_attr_set_status_cb_t)(uint16_t src_addr, uint8_t result);
/* A free-text MSG arrived for this node. `text` is NUL-terminated. */
typedef void (*mesh_attr_text_cb_t)(uint16_t src_addr, const char *text);
/* A node answered a TELEM_GET. `telem` is validated and byte-order corrected;
 * it points at stack memory, so copy anything you need to keep. */
typedef void (*mesh_attr_telem_cb_t)(uint16_t src_addr, const mesh_telem_t *telem);

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
                                  mesh_attr_set_status_cb_t set_status_cb,
                                  mesh_attr_text_cb_t text_cb,
                                  mesh_attr_telem_cb_t telem_cb);

/* --- Gateway side -------------------------------------------------------- */

/* Queries `dst` for the listed attributes; pass ids == NULL / id_count == 0 for
 * all of them. `dst` may be a unicast address or a group address -- a group
 * request reaches every subscriber and each replies unicast after its own
 * random delay. */
esp_err_t mesh_attr_get(uint16_t dst, const dev_attr_id_t *ids, size_t id_count);

/* Asks `dst` for one packed telemetry snapshot. Same addressing rules as
 * mesh_attr_get: unicast, group, or 0xFFFF for everyone, with group replies
 * staggered. Answers arrive on the telem callback. */
esp_err_t mesh_telem_get(uint16_t dst);

/* Writes one attribute on `dst`. Refused at the far end unless the attribute
 * carries DEV_ATTR_FLAG_REMOTE. */
esp_err_t mesh_attr_set(uint16_t dst, dev_attr_id_t id, const char *text);

/* Sends a free-text message to `dst` (unicast, group, or 0xFFFF for all).
 * Unacknowledged: delivery shows up on the receiving node's host link, not
 * here. ESP_ERR_INVALID_SIZE if the text is empty or too long. */
esp_err_t mesh_attr_send_text(uint16_t dst, const char *text);

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
