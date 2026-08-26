/* dev_attr.h - Device attribute registry and TLV codec
 *
 * The bottom layer of the identity stack. Defines *what* attributes a device
 * has and how they are packed onto the wire, and nothing else -- no NVS, no
 * mesh, no I/O of any kind. That keeps it testable in isolation and makes it
 * the single place to extend.
 *
 * ADDING AN ATTRIBUTE: add one row to dev_attr_table[] in dev_attr.c and bump
 * the enum. The NVS store, the wire protocol and the host commands all iterate
 * the table, so nothing else needs to change.
 *
 * Wire format is a flat TLV list:
 *
 *     +------+------+---------------+------+------+-----
 *     |  id  | len  | value (len B) |  id  | len  | ...
 *     +------+------+---------------+------+------+-----
 *
 * Strings are NOT null-terminated on the wire; len is the byte count. u16
 * values are little-endian, matching the mesh convention used elsewhere.
 */

#ifndef _DEV_ATTR_H_
#define _DEV_ATTR_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DEV_ATTR_NAME       = 0x01,
    DEV_ATTR_FW_VER     = 0x02,
    DEV_ATTR_HW_VER     = 0x03,
    DEV_ATTR_GROUP_NAME = 0x04,
    DEV_ATTR_GROUP_ADDR = 0x05,
} dev_attr_id_t;

typedef enum {
    DEV_ATTR_TYPE_STRING,
    DEV_ATTR_TYPE_U16,
} dev_attr_type_t;

/* Who is allowed to write the attribute. Versions carry neither flag: firmware
 * version is owned by the build, hardware version by whoever flashed the board
 * at manufacture (set via DEV_ATTR_FLAG_LOCAL only). */
#define DEV_ATTR_FLAG_LOCAL  (1u << 0)  /* settable over UART/SPI on this board */
#define DEV_ATTR_FLAG_REMOTE (1u << 1)  /* settable by the gateway over the mesh */

/* Longest value any attribute can hold, excluding a null terminator. Bounds the
 * scratch buffers in the layers above. */
#define DEV_ATTR_MAX_VALUE_LEN 31

/* Upper bound on a fully-populated TLV encoding of every attribute. Recomputed
 * by a static assert in dev_attr.c so it cannot silently fall behind the
 * table. */
#define DEV_ATTR_MAX_TLV_LEN 96

typedef struct {
    dev_attr_id_t   id;
    const char     *name;     /* host-facing token, e.g. "NAME" */
    const char     *nvs_key;  /* <= 15 chars, an NVS key length limit */
    dev_attr_type_t type;
    uint8_t         max_len;  /* bytes of value, excluding any terminator */
    uint8_t         flags;
    const char     *def;      /* default as text; parsed for U16 */
} dev_attr_desc_t;

/* --- Registry ------------------------------------------------------------ */

size_t                  dev_attr_count(void);
const dev_attr_desc_t  *dev_attr_at(size_t index);
const dev_attr_desc_t  *dev_attr_find(dev_attr_id_t id);
/* Case-insensitive lookup by host-facing token. NULL if unknown. */
const dev_attr_desc_t  *dev_attr_find_by_name(const char *name);

static inline bool dev_attr_writable_local(const dev_attr_desc_t *d)
{
    return d && (d->flags & DEV_ATTR_FLAG_LOCAL);
}
static inline bool dev_attr_writable_remote(const dev_attr_desc_t *d)
{
    return d && (d->flags & DEV_ATTR_FLAG_REMOTE);
}

/* --- Value helpers ------------------------------------------------------- */

/* Validates a text value against the descriptor: length for strings, and a
 * parseable 0..0xFFFF (decimal or 0x-prefixed) for u16. */
bool dev_attr_value_valid(const dev_attr_desc_t *desc, const char *text);

/* Parses text into the raw wire form. Returns bytes written, or 0 on failure.
 * Strings are copied verbatim; u16 is encoded little-endian. */
size_t dev_attr_value_encode(const dev_attr_desc_t *desc, const char *text,
                             uint8_t *out, size_t out_size);

/* Renders a raw value as text. Always null-terminates when out_size > 0.
 * Returns false if the value is malformed for its type or does not fit. */
bool dev_attr_value_to_text(const dev_attr_desc_t *desc, const uint8_t *value,
                            size_t value_len, char *out, size_t out_size);

/* --- TLV codec ----------------------------------------------------------- */

/* Appends one id/len/value triple at *offset. Returns false if it would not
 * fit, leaving *offset unchanged. */
bool dev_attr_tlv_append(uint8_t *buf, size_t buf_size, size_t *offset,
                         dev_attr_id_t id, const uint8_t *value, size_t value_len);

/* Walks a TLV buffer. Seed *offset to 0 and call until it returns false.
 * On success *value points into buf -- no copy is made. A truncated or
 * over-long record ends iteration rather than reading past the end. */
bool dev_attr_tlv_next(const uint8_t *buf, size_t buf_len, size_t *offset,
                       dev_attr_id_t *id, const uint8_t **value, size_t *value_len);

#ifdef __cplusplus
}
#endif
#endif /* _DEV_ATTR_H_ */
