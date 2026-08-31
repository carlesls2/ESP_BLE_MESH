/* cmd_proto.c - Host command protocol shared by every transport */

#include "cmd_proto.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"

#include "dev_attr.h"
#include "dev_identity.h"
#include "device_mode.h"

#define TAG "CMD"

static void reply(cmd_proto_ctx_t *ctx, const char *text)
{
    ESP_LOGI(TAG, "[%s] %s", ctx->tag, text);
    if (ctx->reply) {
        ctx->reply(text);
    }
}

static void report_mode(cmd_proto_ctx_t *ctx)
{
    char msg[32];
    snprintf(msg, sizeof(msg), "MODE %s", device_mode_name(device_mode_get()));
    reply(ctx, msg);
}

static void apply_mode(cmd_proto_ctx_t *ctx, device_mode_t mode)
{
    esp_err_t err = device_mode_set(mode);
    if (err == ESP_OK) {
        report_mode(ctx);
    } else {
        char msg[64];
        snprintf(msg, sizeof(msg), "ERR %s (staying %s)",
                 esp_err_to_name(err), device_mode_name(device_mode_get()));
        reply(ctx, msg);
    }
}

/* Case-insensitive compare so "mode gateway" works as well as "MODE GATEWAY". */
static bool line_is(const char *line, const char *want)
{
    return strcasecmp(line, want) == 0;
}

/* Matches a leading verb and hands back whatever follows it, with leading
 * whitespace trimmed. The prefix must be followed by a space, so "IDLE" does
 * not match the "ID" verb. */
static bool line_starts_with(const char *line, const char *prefix, const char **rest)
{
    size_t n = strlen(prefix);
    if (strncasecmp(line, prefix, n) != 0 || line[n] != ' ') {
        return false;
    }
    const char *p = line + n;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0') {
        return false;
    }
    *rest = p;
    return true;
}

/* Splits "TOKEN remainder" into an uppercase-insensitive token and the rest.
 * Returns false if there is no token. `tok` is always terminated. */
static bool split_token(const char *in, char *tok, size_t tok_size, const char **rest)
{
    size_t i = 0;
    while (in[i] && in[i] != ' ' && in[i] != '\t') {
        if (i + 1 >= tok_size) {
            return false;
        }
        tok[i] = in[i];
        i++;
    }
    if (i == 0) {
        return false;
    }
    tok[i] = '\0';

    const char *p = in + i;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    *rest = p;
    return true;
}

/* Dumps every attribute in the registry, one reply line each. Iterating the
 * table means a new attribute shows up here with no edit. */
static void report_identity(cmd_proto_ctx_t *ctx)
{
    for (size_t i = 0; i < dev_attr_count(); i++) {
        const dev_attr_desc_t *desc = dev_attr_at(i);
        char value[DEV_ATTR_MAX_VALUE_LEN + 1];
        char msg[DEV_ATTR_MAX_VALUE_LEN + 32];

        if (dev_identity_get_text(desc->id, value, sizeof(value)) == ESP_OK) {
            snprintf(msg, sizeof(msg), "ID %s %s", desc->name, value);
        } else {
            snprintf(msg, sizeof(msg), "ID %s <unreadable>", desc->name);
        }
        reply(ctx, msg);
    }
}

static void report_one_attr(cmd_proto_ctx_t *ctx, const char *attr_name)
{
    const dev_attr_desc_t *desc = dev_attr_find_by_name(attr_name);
    if (desc == NULL) {
        reply(ctx, "ERR unknown attribute");
        return;
    }

    char value[DEV_ATTR_MAX_VALUE_LEN + 1];
    char msg[DEV_ATTR_MAX_VALUE_LEN + 32];
    if (dev_identity_get_text(desc->id, value, sizeof(value)) == ESP_OK) {
        snprintf(msg, sizeof(msg), "ID %s %s", desc->name, value);
    } else {
        snprintf(msg, sizeof(msg), "ID %s <unreadable>", desc->name);
    }
    reply(ctx, msg);
}

static void set_local_attr(cmd_proto_ctx_t *ctx, const char *args)
{
    char        attr[24];
    const char *value = NULL;

    if (!split_token(args, attr, sizeof(attr), &value) || *value == '\0') {
        reply(ctx, "ERR usage: ID SET <attr> <value>");
        return;
    }

    const dev_attr_desc_t *desc = dev_attr_find_by_name(attr);
    if (desc == NULL) {
        reply(ctx, "ERR unknown attribute");
        return;
    }

    esp_err_t err = dev_identity_set_text(desc->id, value, false);
    if (err == ESP_OK) {
        report_one_attr(ctx, desc->name);
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        char msg[64];
        snprintf(msg, sizeof(msg), "ERR %s is read-only", desc->name);
        reply(ctx, msg);
    } else {
        char msg[64];
        snprintf(msg, sizeof(msg), "ERR %s", esp_err_to_name(err));
        reply(ctx, msg);
    }
}

static cmd_mesh_get_fn  s_mesh_get = NULL;
static cmd_mesh_set_fn  s_mesh_set = NULL;
static cmd_mesh_send_fn s_mesh_send = NULL;

void cmd_proto_register_mesh(cmd_mesh_get_fn get_fn, cmd_mesh_set_fn set_fn,
                             cmd_mesh_send_fn send_fn)
{
    s_mesh_get = get_fn;
    s_mesh_set = set_fn;
    s_mesh_send = send_fn;
}

/* "ALL" targets the all-nodes broadcast address; anything else is parsed as an
 * address, so a group address like 0xC001 reaches just that group. */
static bool parse_target(const char *tok, uint16_t *addr)
{
    if (strcasecmp(tok, "ALL") == 0) {
        *addr = 0xFFFF;
        return true;
    }
    char *end = NULL;
    unsigned long v = strtoul(tok, &end, 0);
    if (end == tok || *end != '\0' || v == 0 || v > 0xFFFF) {
        return false;
    }
    *addr = (uint16_t)v;
    return true;
}

static void mesh_err_reply(cmd_proto_ctx_t *ctx, esp_err_t err)
{
    char msg[80];
    if (err == ESP_ERR_INVALID_STATE) {
        snprintf(msg, sizeof(msg), "ERR mesh not ready (promote to gateway first)");
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        snprintf(msg, sizeof(msg), "ERR attribute is not remotely writable");
    } else {
        snprintf(msg, sizeof(msg), "ERR %s", esp_err_to_name(err));
    }
    reply(ctx, msg);
}

/* ASK SET <target> <attr> <value> */
static void ask_remote_set(cmd_proto_ctx_t *ctx, const char *args)
{
    char        target[16];
    char        attr[24];
    const char *rest = NULL;
    const char *value = NULL;

    if (s_mesh_set == NULL) {
        reply(ctx, "ERR mesh not available");
        return;
    }
    if (!split_token(args, target, sizeof(target), &rest) ||
        !split_token(rest, attr, sizeof(attr), &value) || *value == '\0') {
        reply(ctx, "ERR usage: ASK SET <addr> <attr> <value>");
        return;
    }

    uint16_t dst;
    if (!parse_target(target, &dst)) {
        reply(ctx, "ERR bad address");
        return;
    }

    const dev_attr_desc_t *desc = dev_attr_find_by_name(attr);
    if (desc == NULL) {
        reply(ctx, "ERR unknown attribute");
        return;
    }

    esp_err_t err = s_mesh_set(dst, desc->id, value);
    if (err == ESP_OK) {
        char msg[64];
        snprintf(msg, sizeof(msg), "ASK SET 0x%04x %s sent", dst, desc->name);
        reply(ctx, msg);
    } else {
        mesh_err_reply(ctx, err);
    }
}

/* ASK <target> [attr ...] -- results arrive asynchronously on the bridge. */
static void ask_query(cmd_proto_ctx_t *ctx, const char *args)
{
    char        target[16];
    const char *rest = NULL;

    if (s_mesh_get == NULL) {
        reply(ctx, "ERR mesh not available");
        return;
    }
    if (!split_token(args, target, sizeof(target), &rest)) {
        reply(ctx, "ERR usage: ASK <addr|ALL> [attr ...]");
        return;
    }

    uint16_t dst;
    if (!parse_target(target, &dst)) {
        reply(ctx, "ERR bad address");
        return;
    }

    /* No attribute names means everything; naming a few keeps the reply small
     * enough to travel in a single unsegmented message. */
    dev_attr_id_t ids[8];
    size_t        count = 0;
    char          attr[24];

    while (*rest != '\0' && count < sizeof(ids) / sizeof(ids[0])) {
        const char *next = NULL;
        if (!split_token(rest, attr, sizeof(attr), &next)) {
            break;
        }
        const dev_attr_desc_t *desc = dev_attr_find_by_name(attr);
        if (desc == NULL) {
            reply(ctx, "ERR unknown attribute");
            return;
        }
        ids[count++] = desc->id;
        rest = next;
    }

    esp_err_t err = s_mesh_get(dst, count ? ids : NULL, count);
    if (err == ESP_OK) {
        char msg[64];
        snprintf(msg, sizeof(msg), "ASK 0x%04x sent (%u attrs)", dst, (unsigned)count);
        reply(ctx, msg);
    } else {
        mesh_err_reply(ctx, err);
    }
}

/* SEND <target> <text> -- free text to a node's console/host link. */
static void send_text(cmd_proto_ctx_t *ctx, const char *args)
{
    char        target[16];
    const char *text = NULL;

    if (s_mesh_send == NULL) {
        reply(ctx, "ERR mesh not available");
        return;
    }
    if (!split_token(args, target, sizeof(target), &text) || *text == '\0') {
        reply(ctx, "ERR usage: SEND <addr|ALL> <text>");
        return;
    }

    uint16_t dst;
    if (!parse_target(target, &dst)) {
        reply(ctx, "ERR bad address");
        return;
    }

    esp_err_t err = s_mesh_send(dst, text);
    if (err == ESP_OK) {
        char msg[48];
        snprintf(msg, sizeof(msg), "SEND 0x%04x %u chars sent",
                 dst, (unsigned)strlen(text));
        reply(ctx, msg);
    } else if (err == ESP_ERR_INVALID_SIZE) {
        reply(ctx, "ERR text too long");
    } else {
        mesh_err_reply(ctx, err);
    }
}

/* --- Help menu ------------------------------------------------------------ */

/* One row per command form. A verb may span several rows (ASK does); HELP with
 * that verb prints them all. New commands only need a row here to show up. */
typedef struct {
    const char *verb;
    const char *usage;
    const char *desc;
} cmd_help_row_t;

static const cmd_help_row_t s_help_rows[] = {
    { "MODE", "MODE [GATEWAY|NODE]",              "show or switch the device role" },
    { "ID",   "ID? | ID GET <attr>",              "read local identity attributes" },
    { "ID",   "ID SET <attr> <value>",            "write a local identity attribute" },
    { "ASK",  "ASK <addr|ALL> [attr ...]",        "query attributes of remote nodes (gateway)" },
    { "ASK",  "ASK SET <addr> <attr> <value>",    "write an attribute on a remote node (gateway)" },
    { "SEND", "SEND <addr|ALL> <text>",           "send text to remote nodes (gateway)" },
    { "HELP", "HELP [command] | <command> --help", "show this menu" },
};

/* Appends every known attribute name to the menu so "ID SET <attr>" is usable
 * without reading the source. Wraps onto extra lines if the table outgrows one. */
static void report_help_attrs(cmd_proto_ctx_t *ctx)
{
    char   line[CMD_PROTO_MAX_LINE];
    size_t used = (size_t)snprintf(line, sizeof(line), "HELP attrs:");

    for (size_t i = 0; i < dev_attr_count(); i++) {
        const char *name = dev_attr_at(i)->name;
        int n = snprintf(line + used, sizeof(line) - used, " %s", name);
        if (n <= 0 || (size_t)n >= sizeof(line) - used) {
            reply(ctx, line);
            used = (size_t)snprintf(line, sizeof(line), "HELP attrs: %s", name);
            continue;
        }
        used += n;
    }
    reply(ctx, line);
}

/* `what` narrows the menu to one verb ("SEND", or a full line like "ASK SET"
 * whose first token decides); NULL or an unknown verb prints everything. */
static void report_help(cmd_proto_ctx_t *ctx, const char *what)
{
    char        verb[16] = "";
    const char *ignored = NULL;
    bool        matched = false;

    if (what != NULL) {
        split_token(what, verb, sizeof(verb), &ignored);
    }

    for (size_t pass = 0; pass < 2 && !matched; pass++) {
        for (size_t i = 0; i < sizeof(s_help_rows) / sizeof(s_help_rows[0]); i++) {
            const cmd_help_row_t *row = &s_help_rows[i];
            if (pass == 0 && verb[0] != '\0' && strcasecmp(row->verb, verb) != 0) {
                continue;
            }
            char msg[CMD_PROTO_MAX_LINE];
            snprintf(msg, sizeof(msg), "HELP %-33s %s", row->usage, row->desc);
            reply(ctx, msg);
            matched = true;
        }
        /* Unknown verb: fall through to the full menu rather than an error,
         * since whoever asked clearly wants orientation. */
        verb[0] = '\0';
    }

    report_help_attrs(ctx);
}

static void handle_line(cmd_proto_ctx_t *ctx, char *line)
{
    /* Trim surrounding whitespace; a monitor may send stray spaces. */
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t')) {
        line[--n] = '\0';
    }
    if (n == 0) {
        return;
    }

    /* "<command> --help" reads like HELP <command>; strip the flag and route
     * both spellings through the same menu. */
    if (n > 7 && strcasecmp(line + n - 7, " --help") == 0) {
        n -= 7;
        line[n] = '\0';
        while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t')) {
            line[--n] = '\0';
        }
        report_help(ctx, line);
        return;
    }

    const char *rest = NULL;

    if (line_is(line, "HELP") || line_is(line, "--help") || line_is(line, "?")) {
        report_help(ctx, NULL);
    } else if (line_starts_with(line, "HELP", &rest)) {
        report_help(ctx, rest);
    } else if (line_is(line, "MODE GATEWAY")) {
        apply_mode(ctx, DEVICE_MODE_GATEWAY);
    } else if (line_is(line, "MODE NODE")) {
        apply_mode(ctx, DEVICE_MODE_NODE);
    } else if (line_is(line, "MODE?") || line_is(line, "MODE")) {
        report_mode(ctx);
    } else if (line_is(line, "ID?") || line_is(line, "ID")) {
        report_identity(ctx);
    } else if (line_starts_with(line, "ID SET", &rest)) {
        set_local_attr(ctx, rest);
    } else if (line_starts_with(line, "ID GET", &rest)) {
        report_one_attr(ctx, rest);
    } else if (line_starts_with(line, "ASK SET", &rest)) {
        ask_remote_set(ctx, rest);
    } else if (line_starts_with(line, "ASK", &rest)) {
        ask_query(ctx, rest);
    } else if (line_starts_with(line, "SEND", &rest)) {
        send_text(ctx, rest);
    } else {
        reply(ctx, "ERR unknown command (try HELP)");
    }
}

static void handle_binary(cmd_proto_ctx_t *ctx, uint8_t op)
{
    switch (op) {
    case CMD_PROTO_OP_GATEWAY:
        apply_mode(ctx, DEVICE_MODE_GATEWAY);
        break;
    case CMD_PROTO_OP_NODE:
        apply_mode(ctx, DEVICE_MODE_NODE);
        break;
    case CMD_PROTO_OP_QUERY:
        report_mode(ctx);
        break;
    case CMD_PROTO_OP_IDENTITY:
        report_identity(ctx);
        break;
    default:
        ESP_LOGW(TAG, "[%s] unknown binary opcode 0x%02x", ctx->tag, op);
        reply(ctx, "ERR unknown opcode");
        break;
    }
}

void cmd_proto_ctx_init(cmd_proto_ctx_t *ctx, const char *tag, cmd_proto_reply_fn reply_fn)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->tag = tag ? tag : "?";
    ctx->reply = reply_fn;
}

void cmd_proto_feed(cmd_proto_ctx_t *ctx, const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t b = buf[i];

        /* A magic byte starts a 2-byte binary frame. It cannot appear in an
         * ASCII command, so this is unambiguous even mid-line. */
        if (ctx->len == 0 && b == CMD_PROTO_MAGIC) {
            if (i + 1 < len) {
                handle_binary(ctx, buf[++i]);
            } else {
                /* Opcode split across reads: stash the magic and wait. */
                ctx->line[ctx->len++] = (char)b;
            }
            continue;
        }
        if (ctx->len == 1 && (uint8_t)ctx->line[0] == CMD_PROTO_MAGIC) {
            ctx->len = 0;
            handle_binary(ctx, b);
            continue;
        }

        if (b == '\r' || b == '\n') {
            if (ctx->overflow) {
                /* Tail of a line we already gave up on. */
                ctx->overflow = false;
                ctx->len = 0;
                reply(ctx, "ERR command too long");
                continue;
            }
            if (ctx->len > 0) {
                ctx->line[ctx->len] = '\0';
                ctx->len = 0;
                handle_line(ctx, ctx->line);
            }
            continue;
        }

        /* Ignore other control bytes and anything that arrives while we are
         * discarding an over-long line. */
        if (ctx->overflow || b < 0x20 || b > 0x7E) {
            continue;
        }

        if (ctx->len + 1 >= sizeof(ctx->line)) {
            ctx->overflow = true;
            ctx->len = 0;
            continue;
        }
        ctx->line[ctx->len++] = (char)b;
    }
}
