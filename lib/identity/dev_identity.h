/* dev_identity.h - NVS-backed device identity
 *
 * Layer 2 of the identity stack: persists the attributes described by
 * dev_attr.h. Knows about NVS but not about the mesh, so a board with the radio
 * off still has a working identity.
 *
 * Values are cached in RAM after the first load, so reads on the mesh receive
 * path never touch flash.
 */

#ifndef _DEV_IDENTITY_H_
#define _DEV_IDENTITY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "dev_attr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Notified after an attribute is successfully written, so a higher layer can
 * react -- the mesh layer uses this to re-subscribe when GROUP_ADDR changes.
 * Runs on the caller's task with the identity lock released. */
typedef void (*dev_identity_change_cb_t)(dev_attr_id_t id);

/* Loads every attribute from NVS, seeding defaults on a factory-fresh device,
 * and re-mirrors the compile-time firmware version. nvs_flash_init() must have
 * run first. */
esp_err_t dev_identity_init(void);

void dev_identity_register_change_cb(dev_identity_change_cb_t cb);

/* Reads an attribute as text. Always null-terminates when out_size > 0. */
esp_err_t dev_identity_get_text(dev_attr_id_t id, char *out, size_t out_size);

/* Reads an attribute in raw wire form (see dev_attr.h). */
esp_err_t dev_identity_get_raw(dev_attr_id_t id, uint8_t *out, size_t out_size,
                               size_t *out_len);

/* Convenience for the one numeric attribute the mesh layer acts on. */
uint16_t dev_identity_group_addr(void);

/* Writes an attribute from text and persists it. `remote` selects which
 * writability flag is enforced: a mesh-originated write must not be able to
 * change values the local console owns. Returns ESP_ERR_NOT_SUPPORTED if the
 * attribute is not writable by that path. */
esp_err_t dev_identity_set_text(dev_attr_id_t id, const char *text, bool remote);

/* Same, from raw wire form -- used by the mesh SET path so a value does not
 * make a pointless round trip through text. */
esp_err_t dev_identity_set_raw(dev_attr_id_t id, const uint8_t *value,
                               size_t value_len, bool remote);

/* Encodes the requested attributes as a TLV list. Pass ids == NULL (or
 * id_count == 0) for every attribute. Unknown ids are skipped rather than
 * failing the whole reply. Returns bytes written. */
size_t dev_identity_encode_tlv(const dev_attr_id_t *ids, size_t id_count,
                               uint8_t *out, size_t out_size);

#ifdef __cplusplus
}
#endif
#endif /* _DEV_IDENTITY_H_ */
