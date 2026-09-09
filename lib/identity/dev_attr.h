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
 * STORED vs DYNAMIC. A row with no nvs_key is a dynamic attribute: its value is
 * computed on every read -- uptime, free heap, battery -- and never touches
 * flash. Dynamic rows are read-only by construction: they carry neither
 * writability flag and no default. Everything else is stored in NVS and cached
 * in RAM by dev_identity.c.
 *
 * Wire format is a flat TLV list:
 *
 *     +------+------+---------------+------+------+-----
 *     |  id  | len  | value (len B) |  id  | len  | ...
 *     +------+------+---------------+------+------+-----
 *
 * Strings are NOT null-terminated on the wire; len is the byte count. u16/u32
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
    /* Stored identity (NVS-backed). */
    DEV_ATTR_NAME       = 0x01,
    DEV_ATTR_FW_VER     = 0x02,
    DEV_ATTR_HW_VER     = 0x03,
    DEV_ATTR_GROUP_NAME = 0x04,
    DEV_ATTR_GROUP_ADDR = 0x05,

    /* Live telemetry (computed by a provider, never persisted). */
    DEV_ATTR_UPTIME     = 0x06,
    DEV_ATTR_HEAP       = 0x07,
    DEV_ATTR_HEAP_MIN   = 0x08,
    DEV_ATTR_RESET      = 0x09,
    DEV_ATTR_UNICAST    = 0x0A,
    DEV_ATTR_RSSI       = 0x0B,
    DEV_ATTR_BATT_MV    = 0x0C,
    DEV_ATTR_BATT_PCT   = 0x0D,
    DEV_ATTR_ROLE       = 0x0E,
} dev_attr_id_t;

typedef enum {
    DEV_ATTR_TYPE_STRING,
    /* Two bytes rendered as 0xNNNN. Mesh addresses read wrong in decimal. */
    DEV_ATTR_TYPE_ADDR,
    /* Two and four byte unsigned, rendered in decimal -- millivolts, seconds,
     * bytes of heap. Kept distinct from ADDR purely so the text form is right. */
    DEV_ATTR_TYPE_U16,
    DEV_ATTR_TYPE_U32,
    /* Signed single byte, for RSSI. The TLV codec is otherwise unsigned, and
     * offset-encoding a negative into a u16 would misreport on any reader that
     * did not know about the offset. */
    DEV_ATTR_TYPE_S8,
} dev_attr_type_t;

/* Wire width of a fixed-size type; 0 for STRING, which is variable. Lets
 * callers length-check a raw value without switching on the type themselves. */
size_t dev_attr_type_size(dev_attr_type_t type);

/* Who is allowed to write the attribute. Versions carry neither flag: firmware
 * version is owned by the build, hardware version by whoever flashed the board
 * at manufacture (set via DEV_ATTR_FLAG_LOCAL only). Dynamic rows carry neither
 * flag -- there is nowhere to write to. */
#define DEV_ATTR_FLAG_LOCAL  (1u << 0)  /* settable over UART/SPI on this board */
#define DEV_ATTR_FLAG_REMOTE (1u << 1)  /* settable by the gateway over the mesh */

/* Longest value any attribute can hold, excluding a null terminator. Bounds the
 * scratch buffers in the layers above. */
#define DEV_ATTR_MAX_VALUE_LEN 31

/* Upper bound on a fully-populated TLV encoding of every attribute. Recomputed
 * by a static assert in dev_attr.c so it cannot silently fall behind the table.
 * A reply this large segments heavily on the air -- the packed TELEM message in
 * mesh_attr_model.h exists for the common "how is everyone doing" sweep. */
#define DEV_ATTR_MAX_TLV_LEN 176

/* Computes a value on demand, in raw wire form. Returns false if the value is
 * unavailable (no battery divider fitted, address not yet assigned), in which
 * case the attribute is omitted from replies rather than reported as zero.
 * Runs on the caller's task -- keep it cheap and non-blocking, since the mesh
 * receive path calls it. */
typedef bool (*dev_attr_provider_fn)(uint8_t *out, size_t out_size, size_t *out_len);

typedef struct {
    dev_attr_id_t   id;
    const char     *name;     /* host-facing token, e.g. "NAME" */
    const char     *nvs_key;  /* <= 15 chars, an NVS key length limit; NULL if dynamic */
    dev_attr_type_t type;
    uint8_t         max_len;  /* bytes of value, excluding any terminator */
    uint8_t         flags;
    const char     *def;      /* default as text; parsed for the numeric types */
} dev_attr_desc_t;

/* --- Registry ------------------------------------------------------------ */

size_t                  dev_attr_count(void);
const dev_attr_desc_t  *dev_attr_at(size_t index);
const dev_attr_desc_t  *dev_attr_find(dev_attr_id_t id);
/* Case-insensitive lookup by host-facing token. NULL if unknown. */
const dev_attr_desc_t  *dev_attr_find_by_name(const char *name);

/* A row with no nvs_key is computed, not stored. */
static inline bool dev_attr_is_dynamic(const dev_attr_desc_t *d)
{
    return d && (d->nvs_key == NULL);
}
static inline bool dev_attr_writable_local(const dev_attr_desc_t *d)
{
    return d && (d->flags & DEV_ATTR_FLAG_LOCAL);
}
static inline bool dev_attr_writable_remote(const dev_attr_desc_t *d)
{
    return d && (d->flags & DEV_ATTR_FLAG_REMOTE);
}

/* --- Providers ----------------------------------------------------------- */

/* Installs the function backing one dynamic row. Called at boot by
 * telemetry_init(); until then a dynamic row simply reports nothing. Keeping
 * the wiring out of the table is what lets lib/identity stay free of any
 * dependency on lib/telemetry -- the same inversion cmd_proto.h uses to stay
 * free of lib/BLE. Registering against a stored row is rejected.
 *
 * Returns false if the id is unknown or names a stored row. Deliberately not
 * esp_err_t: this header pulls in no ESP-IDF dependency, which is what keeps
 * the registry testable on a host compiler. */
bool dev_attr_set_provider(dev_attr_id_t id, dev_attr_provider_fn fn);

/* Runs the provider for `desc`. False if the row is stored, has no provider
 * registered yet, or the provider itself declined. */
bool dev_attr_provide(const dev_attr_desc_t *desc, uint8_t *out, size_t out_size,
                      size_t *out_len);

/* --- Value helpers ------------------------------------------------------- */

/* Validates a text value against the descriptor: length for strings, and a
 * parseable range (decimal or 0x-prefixed) for the numeric types. */
bool dev_attr_value_valid(const dev_attr_desc_t *desc, const char *text);

/* Parses text into the raw wire form. Returns bytes written, or 0 on failure.
 * Strings are copied verbatim; numbers are encoded little-endian. */
size_t dev_attr_value_encode(const dev_attr_desc_t *desc, const char *text,
                             uint8_t *out, size_t out_size);

/* Renders a raw value as text. Always null-terminates when out_size > 0.
 * Returns false if the value is malformed for its type or does not fit. */
bool dev_attr_value_to_text(const dev_attr_desc_t *desc, const uint8_t *value,
                            size_t value_len, char *out, size_t out_size);

/* --- Raw encode helpers, for provider implementations -------------------- */

bool dev_attr_put_u16(uint16_t v, uint8_t *out, size_t out_size, size_t *out_len);
bool dev_attr_put_u32(uint32_t v, uint8_t *out, size_t out_size, size_t *out_len);
bool dev_attr_put_s8(int8_t v, uint8_t *out, size_t out_size, size_t *out_len);
bool dev_attr_put_str(const char *s, uint8_t *out, size_t out_size, size_t *out_len);

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
