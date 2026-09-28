/* dev_attr.c - Device attribute registry and TLV codec */

#include "dev_attr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Firmware version is owned by the build, not by NVS. It is mirrored into NVS
 * on every boot so a query can never report a version the board is not running.
 * Override from platformio.ini with -DAPP_FW_VERSION=\"x.y.z\" if you wire this
 * to git describe later. */
#ifndef APP_FW_VERSION
#define APP_FW_VERSION "0.5.0"
#endif

#ifndef APP_HW_VERSION
#define APP_HW_VERSION "esp32-wroom-32"
#endif

/* THE EXTENSION POINT.
 *
 * One row per attribute. Everything above this layer iterates the table, so a
 * new attribute needs no other edit -- storage, wire format and host commands
 * all pick it up.
 *
 * nvs_key must be <= 15 characters (NVS limit). max_len must be
 * <= DEV_ATTR_MAX_VALUE_LEN. A row with nvs_key == NULL is dynamic: it is
 * computed by a provider registered at boot, never stored, never writable. */
static const dev_attr_desc_t dev_attr_table[] = {
    { DEV_ATTR_NAME,       "NAME",       "name",     DEV_ATTR_TYPE_STRING, 31,
      DEV_ATTR_FLAG_LOCAL | DEV_ATTR_FLAG_REMOTE, "unnamed" },

    { DEV_ATTR_FW_VER,     "FW_VER",     "fw_ver",   DEV_ATTR_TYPE_STRING, 15,
      0, APP_FW_VERSION },

    { DEV_ATTR_HW_VER,     "HW_VER",     "hw_ver",   DEV_ATTR_TYPE_STRING, 15,
      DEV_ATTR_FLAG_LOCAL, APP_HW_VERSION },

    { DEV_ATTR_GROUP_NAME, "GROUP_NAME", "grp_name", DEV_ATTR_TYPE_STRING, 15,
      DEV_ATTR_FLAG_LOCAL | DEV_ATTR_FLAG_REMOTE, "default" },

    /* 0x0000 means "unassigned"; a real group address is 0xC000-0xFFFF. */
    { DEV_ATTR_GROUP_ADDR, "GROUP_ADDR", "grp_addr", DEV_ATTR_TYPE_ADDR, 2,
      DEV_ATTR_FLAG_LOCAL | DEV_ATTR_FLAG_REMOTE, "0x0000" },

    /* --- Dynamic rows. nvs_key and def are NULL; see dev_attr_is_dynamic. --- */

    { DEV_ATTR_UPTIME,     "UPTIME",     NULL,       DEV_ATTR_TYPE_U32,    4, 0, NULL },
    { DEV_ATTR_HEAP,       "HEAP",       NULL,       DEV_ATTR_TYPE_U32,    4, 0, NULL },
    { DEV_ATTR_HEAP_MIN,   "HEAP_MIN",   NULL,       DEV_ATTR_TYPE_U32,    4, 0, NULL },
    { DEV_ATTR_RESET,      "RESET",      NULL,       DEV_ATTR_TYPE_STRING, 15, 0, NULL },
    { DEV_ATTR_UNICAST,    "UNICAST",    NULL,       DEV_ATTR_TYPE_ADDR,   2, 0, NULL },
    { DEV_ATTR_RSSI,       "RSSI",       NULL,       DEV_ATTR_TYPE_S8,     1, 0, NULL },
    { DEV_ATTR_BATT_MV,    "BATT_MV",    NULL,       DEV_ATTR_TYPE_U16,    2, 0, NULL },
    { DEV_ATTR_BATT_PCT,   "BATT_PCT",   NULL,       DEV_ATTR_TYPE_U16,    2, 0, NULL },
    { DEV_ATTR_ROLE,       "ROLE",       NULL,       DEV_ATTR_TYPE_STRING, 15, 0, NULL },
};

#define DEV_ATTR_TABLE_LEN (sizeof(dev_attr_table) / sizeof(dev_attr_table[0]))

/* Providers live beside the table rather than in it, so the table stays const
 * and lib/identity keeps no link-time dependency on whatever supplies the
 * values. Indexed identically to dev_attr_table. */
static dev_attr_provider_fn s_providers[DEV_ATTR_TABLE_LEN];

/* Worst case is every attribute present, each costing 2 header bytes. Keeps
 * DEV_ATTR_MAX_TLV_LEN honest as the table grows -- the sum below must list one
 * max_len per row, in table order. */
_Static_assert(/* stored  */ 31 + 15 + 15 + 15 + 2 +
               /* dynamic */ 4 + 4 + 4 + 15 + 2 + 1 + 2 + 2 + 15 +
               (2 * DEV_ATTR_TABLE_LEN) <= DEV_ATTR_MAX_TLV_LEN,
               "DEV_ATTR_MAX_TLV_LEN is too small for dev_attr_table");

size_t dev_attr_count(void)
{
    return DEV_ATTR_TABLE_LEN;
}

const dev_attr_desc_t *dev_attr_at(size_t index)
{
    return (index < DEV_ATTR_TABLE_LEN) ? &dev_attr_table[index] : NULL;
}

const dev_attr_desc_t *dev_attr_find(dev_attr_id_t id)
{
    for (size_t i = 0; i < DEV_ATTR_TABLE_LEN; i++) {
        if (dev_attr_table[i].id == id) {
            return &dev_attr_table[i];
        }
    }
    return NULL;
}

const dev_attr_desc_t *dev_attr_find_by_name(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < DEV_ATTR_TABLE_LEN; i++) {
        if (strcasecmp(dev_attr_table[i].name, name) == 0) {
            return &dev_attr_table[i];
        }
    }
    return NULL;
}

size_t dev_attr_type_size(dev_attr_type_t type)
{
    switch (type) {
    case DEV_ATTR_TYPE_S8:   return 1;
    case DEV_ATTR_TYPE_ADDR: return 2;
    case DEV_ATTR_TYPE_U16:  return 2;
    case DEV_ATTR_TYPE_U32:  return 4;
    default:                 return 0;   /* STRING is variable-length */
    }
}

/* --- Providers ----------------------------------------------------------- */

bool dev_attr_set_provider(dev_attr_id_t id, dev_attr_provider_fn fn)
{
    for (size_t i = 0; i < DEV_ATTR_TABLE_LEN; i++) {
        if (dev_attr_table[i].id != id) {
            continue;
        }
        /* Refuse to shadow a stored value: a provider on an NVS-backed row
         * would make writes silently invisible on read-back. */
        if (!dev_attr_is_dynamic(&dev_attr_table[i])) {
            return false;
        }
        s_providers[i] = fn;
        return true;
    }
    return false;
}

bool dev_attr_provide(const dev_attr_desc_t *desc, uint8_t *out, size_t out_size,
                      size_t *out_len)
{
    if (desc == NULL || out == NULL || out_len == NULL) {
        return false;
    }
    for (size_t i = 0; i < DEV_ATTR_TABLE_LEN; i++) {
        if (&dev_attr_table[i] == desc) {
            return s_providers[i] ? s_providers[i](out, out_size, out_len) : false;
        }
    }
    return false;
}

/* --- Numeric parsing ----------------------------------------------------- */

/* Accepts decimal or 0x-prefixed hex, rejecting trailing junk and anything
 * above `limit`. */
static bool parse_uint(const char *text, unsigned long limit, unsigned long *out)
{
    if (text == NULL || *text == '\0') {
        return false;
    }

    char *end = NULL;
    unsigned long v = strtoul(text, &end, 0);
    if (end == text || *end != '\0' || v > limit) {
        return false;
    }
    *out = v;
    return true;
}

/* Signed, for S8. Kept separate because strtoul would wrap "-63" into a huge
 * positive rather than rejecting it. */
static bool parse_s8(const char *text, int8_t *out)
{
    if (text == NULL || *text == '\0') {
        return false;
    }

    char *end = NULL;
    long v = strtol(text, &end, 0);
    if (end == text || *end != '\0' || v < -128 || v > 127) {
        return false;
    }
    *out = (int8_t)v;
    return true;
}

bool dev_attr_value_valid(const dev_attr_desc_t *desc, const char *text)
{
    if (desc == NULL || text == NULL) {
        return false;
    }

    unsigned long scratch;
    int8_t        s8;

    switch (desc->type) {
    case DEV_ATTR_TYPE_ADDR:
    case DEV_ATTR_TYPE_U16:
        return parse_uint(text, 0xFFFFUL, &scratch);
    case DEV_ATTR_TYPE_U32:
        return parse_uint(text, 0xFFFFFFFFUL, &scratch);
    case DEV_ATTR_TYPE_S8:
        return parse_s8(text, &s8);
    default:
        break;
    }

    size_t len = strlen(text);
    return len > 0 && len <= desc->max_len;
}

size_t dev_attr_value_encode(const dev_attr_desc_t *desc, const char *text,
                             uint8_t *out, size_t out_size)
{
    if (!dev_attr_value_valid(desc, text) || out == NULL) {
        return 0;
    }

    size_t        width = dev_attr_type_size(desc->type);
    unsigned long v     = 0;
    int8_t        s8    = 0;

    if (width > 0) {
        if (out_size < width) {
            return 0;
        }
        if (desc->type == DEV_ATTR_TYPE_S8) {
            if (!parse_s8(text, &s8)) {
                return 0;
            }
            out[0] = (uint8_t)s8;
            return 1;
        }
        if (!parse_uint(text, (width == 4) ? 0xFFFFFFFFUL : 0xFFFFUL, &v)) {
            return 0;
        }
        for (size_t i = 0; i < width; i++) {
            out[i] = (uint8_t)((v >> (8 * i)) & 0xFF);   /* little-endian */
        }
        return width;
    }

    size_t len = strlen(text);
    if (len > out_size) {
        return 0;
    }
    memcpy(out, text, len);
    return len;
}

bool dev_attr_value_to_text(const dev_attr_desc_t *desc, const uint8_t *value,
                            size_t value_len, char *out, size_t out_size)
{
    if (desc == NULL || value == NULL || out == NULL || out_size == 0) {
        return false;
    }

    size_t width = dev_attr_type_size(desc->type);
    if (width > 0) {
        if (value_len != width) {
            out[0] = '\0';
            return false;
        }

        uint32_t v = 0;
        for (size_t i = 0; i < width; i++) {
            v |= (uint32_t)value[i] << (8 * i);
        }

        int n;
        switch (desc->type) {
        case DEV_ATTR_TYPE_ADDR:
            n = snprintf(out, out_size, "0x%04x", (unsigned)(v & 0xFFFF));
            break;
        case DEV_ATTR_TYPE_S8:
            n = snprintf(out, out_size, "%d", (int)(int8_t)value[0]);
            break;
        default:
            n = snprintf(out, out_size, "%u", (unsigned)v);
            break;
        }
        return n > 0 && n < (int)out_size;
    }

    if (value_len >= out_size) {
        out[0] = '\0';
        return false;
    }
    memcpy(out, value, value_len);
    out[value_len] = '\0';
    return true;
}

/* --- Raw encode helpers, for provider implementations -------------------- */

bool dev_attr_put_u16(uint16_t v, uint8_t *out, size_t out_size, size_t *out_len)
{
    if (out == NULL || out_len == NULL || out_size < 2) {
        return false;
    }
    out[0] = (uint8_t)(v & 0xFF);
    out[1] = (uint8_t)(v >> 8);
    *out_len = 2;
    return true;
}

bool dev_attr_put_u32(uint32_t v, uint8_t *out, size_t out_size, size_t *out_len)
{
    if (out == NULL || out_len == NULL || out_size < 4) {
        return false;
    }
    for (size_t i = 0; i < 4; i++) {
        out[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    }
    *out_len = 4;
    return true;
}

bool dev_attr_put_s8(int8_t v, uint8_t *out, size_t out_size, size_t *out_len)
{
    if (out == NULL || out_len == NULL || out_size < 1) {
        return false;
    }
    out[0] = (uint8_t)v;
    *out_len = 1;
    return true;
}

bool dev_attr_put_str(const char *s, uint8_t *out, size_t out_size, size_t *out_len)
{
    if (s == NULL || out == NULL || out_len == NULL) {
        return false;
    }
    size_t len = strlen(s);
    if (len == 0 || len > out_size) {
        return false;
    }
    memcpy(out, s, len);
    *out_len = len;
    return true;
}

/* --- TLV codec ----------------------------------------------------------- */

bool dev_attr_tlv_append(uint8_t *buf, size_t buf_size, size_t *offset,
                         dev_attr_id_t id, const uint8_t *value, size_t value_len)
{
    if (buf == NULL || offset == NULL || value_len > 0xFF) {
        return false;
    }
    if (value_len > 0 && value == NULL) {
        return false;
    }
    if (*offset + 2 + value_len > buf_size) {
        return false;
    }

    buf[(*offset)++] = (uint8_t)id;
    buf[(*offset)++] = (uint8_t)value_len;
    if (value_len > 0) {
        memcpy(&buf[*offset], value, value_len);
        *offset += value_len;
    }
    return true;
}

bool dev_attr_tlv_next(const uint8_t *buf, size_t buf_len, size_t *offset,
                       dev_attr_id_t *id, const uint8_t **value, size_t *value_len)
{
    if (buf == NULL || offset == NULL || id == NULL || value == NULL || value_len == NULL) {
        return false;
    }
    /* Need both header bytes before we can trust the length. */
    if (*offset + 2 > buf_len) {
        return false;
    }

    uint8_t rec_id  = buf[*offset];
    uint8_t rec_len = buf[*offset + 1];

    /* A record claiming more than remains is truncated -- stop rather than
     * read past the buffer. */
    if (*offset + 2 + rec_len > buf_len) {
        return false;
    }

    *id        = (dev_attr_id_t)rec_id;
    *value     = &buf[*offset + 2];
    *value_len = rec_len;
    *offset   += 2 + rec_len;
    return true;
}
