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
#define APP_FW_VERSION "0.3.0"
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
 * <= DEV_ATTR_MAX_VALUE_LEN. */
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
    { DEV_ATTR_GROUP_ADDR, "GROUP_ADDR", "grp_addr", DEV_ATTR_TYPE_U16, 2,
      DEV_ATTR_FLAG_LOCAL | DEV_ATTR_FLAG_REMOTE, "0x0000" },
};

#define DEV_ATTR_TABLE_LEN (sizeof(dev_attr_table) / sizeof(dev_attr_table[0]))

/* Worst case is every attribute present, each costing 2 header bytes. Keeps
 * DEV_ATTR_MAX_TLV_LEN honest as the table grows. */
_Static_assert(31 + 15 + 15 + 15 + 2 + (2 * DEV_ATTR_TABLE_LEN) <= DEV_ATTR_MAX_TLV_LEN,
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

/* Accepts decimal or 0x-prefixed hex, rejecting trailing junk and anything
 * outside 0..0xFFFF. */
static bool parse_u16(const char *text, uint16_t *out)
{
    if (text == NULL || *text == '\0') {
        return false;
    }

    char *end = NULL;
    unsigned long v = strtoul(text, &end, 0);
    if (end == text || *end != '\0' || v > 0xFFFFUL) {
        return false;
    }
    *out = (uint16_t)v;
    return true;
}

bool dev_attr_value_valid(const dev_attr_desc_t *desc, const char *text)
{
    if (desc == NULL || text == NULL) {
        return false;
    }

    if (desc->type == DEV_ATTR_TYPE_U16) {
        uint16_t scratch;
        return parse_u16(text, &scratch);
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

    if (desc->type == DEV_ATTR_TYPE_U16) {
        uint16_t v;
        if (out_size < 2 || !parse_u16(text, &v)) {
            return 0;
        }
        out[0] = (uint8_t)(v & 0xFF);
        out[1] = (uint8_t)(v >> 8);
        return 2;
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

    if (desc->type == DEV_ATTR_TYPE_U16) {
        if (value_len != 2) {
            out[0] = '\0';
            return false;
        }
        uint16_t v = (uint16_t)value[0] | ((uint16_t)value[1] << 8);
        return snprintf(out, out_size, "0x%04x", v) < (int)out_size;
    }

    if (value_len >= out_size) {
        out[0] = '\0';
        return false;
    }
    memcpy(out, value, value_len);
    out[value_len] = '\0';
    return true;
}

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
