/* cmd_proto.c - Host command protocol shared by every transport */

#include "cmd_proto.h"

#include <stdio.h>
#include <stdlib.h>
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

/* Renders one "ID <NAME> <value>" line.
 *
 * A dynamic attribute whose provider declined reports "-": no battery divider
 * fitted, nothing heard yet for RSSI, no address until provisioned. That is a
 * normal answer, not a failure, and it must not read like corruption --
 * "<unreadable>" stays reserved for a stored value that really is malformed. */
static void format_attr_line(const dev_attr_desc_t *desc, char *msg, size_t msg_size)
{
    char value[DEV_ATTR_MAX_VALUE_LEN + 1];

    if (dev_identity_get_text(desc->id, value, sizeof(value)) == ESP_OK) {
        snprintf(msg, msg_size, "ID %s %s", desc->name, value);
    } else if (dev_attr_is_dynamic(desc)) {
        snprintf(msg, msg_size, "ID %s -", desc->name);
    } else {
        snprintf(msg, msg_size, "ID %s <unreadable>", desc->name);
    }
}

/* Dumps every attribute in the registry, one reply line each. Iterating the
 * table means a new attribute shows up here with no edit. */
static void report_identity(cmd_proto_ctx_t *ctx)
{
    for (size_t i = 0; i < dev_attr_count(); i++) {
        char msg[DEV_ATTR_MAX_VALUE_LEN + 32];
        format_attr_line(dev_attr_at(i), msg, sizeof(msg));
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

    char msg[DEV_ATTR_MAX_VALUE_LEN + 32];
    format_attr_line(desc, msg, sizeof(msg));
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

static const cmd_mesh_ops_t *s_mesh = NULL;

void cmd_proto_register_mesh(const cmd_mesh_ops_t *ops)
{
    s_mesh = ops;
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

    if (s_mesh == NULL || s_mesh->set == NULL) {
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

    esp_err_t err = s_mesh->set(dst, desc->id, value);
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

    if (s_mesh == NULL || s_mesh->get == NULL) {
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

    esp_err_t err = s_mesh->get(dst, count ? ids : NULL, count);
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

    if (s_mesh == NULL || s_mesh->send == NULL) {
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

    esp_err_t err = s_mesh->send(dst, text);
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

/* TELEM <target> -- one packed vitals snapshot per node, answered
 * asynchronously on the bridge as a TELEM line. Cheaper on the air than
 * ASK <addr> with every attribute named; see mesh_attr_model.h. */
static void telem_query(cmd_proto_ctx_t *ctx, const char *args)
{
    char        target[16];
    const char *ignored = NULL;

    if (s_mesh == NULL || s_mesh->telem == NULL) {
        reply(ctx, "ERR mesh not available");
        return;
    }
    if (!split_token(args, target, sizeof(target), &ignored)) {
        reply(ctx, "ERR usage: TELEM <addr|ALL>");
        return;
    }

    uint16_t dst;
    if (!parse_target(target, &dst)) {
        reply(ctx, "ERR bad address");
        return;
    }

    esp_err_t err = s_mesh->telem(dst);
    if (err == ESP_OK) {
        char msg[48];
        snprintf(msg, sizeof(msg), "TELEM 0x%04x sent", dst);
        reply(ctx, msg);
    } else {
        mesh_err_reply(ctx, err);
    }
}

/* NODES -- who has this gateway provisioned? Read straight out of the stack's
 * own table, which CONFIG_BLE_MESH_SETTINGS persists, so the answer survives a
 * reboot of both the gateway and whatever host is asking. */
static void list_nodes(cmd_proto_ctx_t *ctx)
{
    if (s_mesh == NULL || s_mesh->nodes == NULL) {
        reply(ctx, "ERR mesh not available");
        return;
    }
    if (ctx->reply == NULL) {
        /* Nowhere to write the list; refuse rather than half-answer. */
        reply(ctx, "ERR transport has no reply channel");
        return;
    }

    esp_err_t err = s_mesh->nodes(ctx->reply);
    if (err != ESP_OK) {
        mesh_err_reply(ctx, err);
    }
}

/* NEIGHBORS -- this board's own table, answered on the spot. Works in NODE mode
 * too: every board keeps a table, gateway or not. */
static void neighbors_local(cmd_proto_ctx_t *ctx)
{
    if (s_mesh == NULL || s_mesh->neigh_local == NULL) {
        reply(ctx, "ERR mesh not available");
        return;
    }
    if (ctx->reply == NULL) {
        reply(ctx, "ERR transport has no reply channel");
        return;
    }

    esp_err_t err = s_mesh->neigh_local(ctx->reply);
    if (err != ESP_OK) {
        mesh_err_reply(ctx, err);
    }
}

/* NEIGHBORS <addr|ALL> -- a remote node's table, via the gateway. Results
 * arrive asynchronously on the bridge. */
static void neighbors_query(cmd_proto_ctx_t *ctx, const char *args)
{
    char        target[16];
    const char *ignored = NULL;

    if (s_mesh == NULL || s_mesh->neigh_query == NULL) {
        reply(ctx, "ERR mesh not available");
        return;
    }
    if (!split_token(args, target, sizeof(target), &ignored)) {
        reply(ctx, "ERR usage: NEIGHBORS [addr|ALL]");
        return;
    }

    uint16_t dst;
    if (!parse_target(target, &dst) ||
        (dst != 0xFFFF && (dst & 0x8000) != 0)) {
        reply(ctx, "ERR bad address (unicast or ALL)");
        return;
    }

    esp_err_t err = s_mesh->neigh_query(dst);
    if (err == ESP_OK) {
        char msg[48];
        if (dst == 0xFFFF) {
            snprintf(msg, sizeof(msg), "NEIGHBORS ALL started");
        } else {
            snprintf(msg, sizeof(msg), "NEIGHBORS 0x%04x sent", dst);
        }
        reply(ctx, msg);
    } else if (err == ESP_ERR_INVALID_ARG) {
        /* The gateway's own address: nothing to ask over the air. */
        neighbors_local(ctx);
    } else if (err == ESP_ERR_NOT_FINISHED) {
        reply(ctx, "ERR a NEIGHBORS ALL sweep is already running");
    } else {
        mesh_err_reply(ctx, err);
    }
}

/* --- RTC, time and sleep ---------------------------------------------------
 *
 * Every verb here works on this board, or -- with a 0x.... / ALL address
 * right after the verb -- on remote nodes through the gateway. Local answers
 * use the same line a remote node's answer produces on the bridge:
 *
 *     RTC <addr> <OK|BAD_ARG|REFUSED|FAILED> time=... valid=... backend=...
 *                wake=... duty=...
 */

/* A leading "0x...." or "ALL" token selects a remote target. Plain numbers
 * are deliberately not addresses here: "TIME SET 1790000000" is an epoch. */
static bool take_remote(const char *args, uint16_t *dst, const char **rest)
{
    char        tok[16];
    const char *after = NULL;

    if (!split_token(args, tok, sizeof(tok), &after)) {
        return false;
    }
    if (strncasecmp(tok, "0x", 2) != 0 && strcasecmp(tok, "ALL") != 0) {
        return false;
    }
    if (!parse_target(tok, dst)) {
        return false;
    }
    *rest = after;
    return true;
}

static void rtc_report_local(cmd_proto_ctx_t *ctx, esp_err_t result)
{
    rtc_status_t st;
    char         status[112];
    char         addr[DEV_ATTR_MAX_VALUE_LEN + 1];
    char         msg[160];

    if (dev_identity_get_text(DEV_ATTR_UNICAST, addr, sizeof(addr)) != ESP_OK) {
        snprintf(addr, sizeof(addr), "0x0000");
    }
    rtc_mgr_status(&st);
    rtc_mgr_format_status(&st, status, sizeof(status));
    snprintf(msg, sizeof(msg), "RTC %s %s %s", addr,
             rtc_mgr_result_name(rtc_mgr_result_code(result)), status);
    reply(ctx, msg);
}

/* Says why a local request was refused, in words a person can act on. */
static void rtc_err_reply(cmd_proto_ctx_t *ctx, const rtc_req_t *req, esp_err_t err)
{
    if (err == ESP_ERR_INVALID_STATE) {
        if (device_mode_get() == DEVICE_MODE_GATEWAY &&
            (req->op == RTC_REQ_SLEEP || req->op == RTC_REQ_DUTY)) {
            reply(ctx, "ERR a gateway does not sleep");
        } else {
            reply(ctx, "ERR clock not set (TIME SET first)");
        }
    } else if (err == ESP_ERR_INVALID_ARG) {
        char msg[64];
        snprintf(msg, sizeof(msg), "ERR out of range (awake >= %u s, sleep <= 7 days)",
                 (unsigned)RTC_DUTY_MIN_AWAKE_S);
        reply(ctx, msg);
    } else {
        mesh_err_reply(ctx, err);
    }
}

/* Parses the part of TIME SET / SLEEP / DUTY after the optional address into
 * a request. `verb` picks the grammar. Replies with ERR and returns false on a
 * malformed line. */
static bool parse_rtc_req(cmd_proto_ctx_t *ctx, const char *verb, const char *args,
                          rtc_req_t *req)
{
    char        tok[32];
    const char *rest = NULL;
    time_t      now;
    bool        valid;

    rtc_mgr_get_time(&now, &valid);
    memset(req, 0, sizeof(*req));

    if (strcasecmp(verb, "TIME") == 0) {
        time_t t;
        if (!rtc_mgr_parse_time(args, now, &t)) {
            reply(ctx, "ERR usage: TIME SET [addr|ALL] <epoch|YYYY-MM-DDTHH:MM:SS>");
            return false;
        }
        req->op = RTC_REQ_TIME_SET;
        req->a = (uint32_t)t;
        return true;
    }

    if (!split_token(args, tok, sizeof(tok), &rest)) {
        goto usage;
    }

    if (strcasecmp(verb, "DUTY") == 0 && strcasecmp(tok, "OFF") == 0) {
        req->op = RTC_REQ_DUTY_OFF;
        return true;
    }
    if (!rtc_mgr_parse_mode(tok, &req->mode)) {
        goto usage;
    }

    if (strcasecmp(verb, "SLEEP") == 0) {
        req->op = RTC_REQ_SLEEP;
        if (!split_token(rest, tok, sizeof(tok), &rest)) {
            goto usage;
        }
        if (strcasecmp(tok, "UNTIL") == 0) {
            time_t t;
            if (!rtc_mgr_parse_time(rest, now, &t)) {
                goto usage;
            }
            req->absolute = true;
            req->a = (uint32_t)t;
            return true;
        }
        char *end;
        req->a = (uint32_t)strtoul(tok, &end, 10);
        if (*end != '\0') {
            goto usage;
        }
        return true;
    }

    /* DUTY <mode> <awake_s> <sleep_s> [ALIGN] */
    req->op = RTC_REQ_DUTY;
    char  a[12], b[12];
    char *end_a, *end_b;
    if (!split_token(rest, a, sizeof(a), &rest) || !split_token(rest, b, sizeof(b), &rest)) {
        goto usage;
    }
    req->a = (uint32_t)strtoul(a, &end_a, 10);
    req->b = (uint32_t)strtoul(b, &end_b, 10);
    if (*end_a != '\0' || *end_b != '\0') {
        goto usage;
    }
    if (*rest != '\0') {
        if (strcasecmp(rest, "ALIGN") != 0) {
            goto usage;
        }
        req->absolute = true;
    }
    return true;

usage:
    if (strcasecmp(verb, "SLEEP") == 0) {
        reply(ctx, "ERR usage: SLEEP [addr|ALL] <LIGHT|DEEP> <seconds | UNTIL time>");
    } else {
        reply(ctx, "ERR usage: DUTY [addr|ALL] <LIGHT|DEEP> <awake_s> <sleep_s> [ALIGN] | OFF");
    }
    return false;
}

static void rtc_send_remote(cmd_proto_ctx_t *ctx, uint16_t dst, const rtc_req_t *req)
{
    if (s_mesh == NULL || s_mesh->rtc == NULL) {
        reply(ctx, "ERR mesh not available");
        return;
    }
    esp_err_t err = s_mesh->rtc(dst, req);
    if (err == ESP_OK) {
        char msg[32];
        snprintf(msg, sizeof(msg), "RTC 0x%04x sent", dst);
        reply(ctx, msg);
    } else {
        mesh_err_reply(ctx, err);
    }
}

/* RTC | RTC? | RTC <addr|ALL> */
static void rtc_command(cmd_proto_ctx_t *ctx, const char *args)
{
    uint16_t    dst;
    const char *rest = NULL;

    if (args == NULL) {
        rtc_report_local(ctx, ESP_OK);
        return;
    }
    if (!take_remote(args, &dst, &rest) || *rest != '\0') {
        reply(ctx, "ERR usage: RTC [addr|ALL]");
        return;
    }
    rtc_req_t req = { .op = RTC_REQ_GET };
    rtc_send_remote(ctx, dst, &req);
}

/* TIME SET ... / TIME SYNC <addr|ALL> / SLEEP ... / DUTY ... */
static void rtc_verb(cmd_proto_ctx_t *ctx, const char *verb, const char *args)
{
    uint16_t    dst;
    const char *rest = NULL;
    bool        remote = take_remote(args, &dst, &rest);
    rtc_req_t   req;

    if (strcasecmp(verb, "SYNC") == 0) {
        /* Push this board's clock to others: the one way to set a whole
         * network from a single trusted source. */
        time_t now;
        bool   valid;
        rtc_mgr_get_time(&now, &valid);
        if (!remote || *rest != '\0') {
            reply(ctx, "ERR usage: TIME SYNC <addr|ALL>");
            return;
        }
        if (!valid) {
            reply(ctx, "ERR clock not set (TIME SET first)");
            return;
        }
        req = (rtc_req_t){ .op = RTC_REQ_TIME_SET, .a = (uint32_t)now };
        rtc_send_remote(ctx, dst, &req);
        return;
    }

    if (!parse_rtc_req(ctx, verb, remote ? rest : args, &req)) {
        return;
    }
    if (remote) {
        rtc_send_remote(ctx, dst, &req);
        return;
    }

    esp_err_t err = rtc_mgr_apply(&req);
    if (err != ESP_OK) {
        rtc_err_reply(ctx, &req, err);
    } else if (req.op == RTC_REQ_SLEEP) {
        char msg[64];
        snprintf(msg, sizeof(msg), "SLEEP %s %s %u (in %u ms)",
                 rtc_mgr_mode_name(req.mode), req.absolute ? "until" : "for",
                 (unsigned)req.a, (unsigned)RTC_SLEEP_GRACE_MS);
        reply(ctx, msg);
    } else {
        rtc_report_local(ctx, ESP_OK);
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
    { "TELEM","TELEM <addr|ALL>",                 "live vitals: uptime, heap, battery, RSSI (gateway)" },
    { "NODES","NODES",                            "list nodes this gateway has provisioned" },
    { "NEIGHBORS", "NEIGHBORS",                   "nodes this board hears directly (RSSI, age)" },
    { "NEIGHBORS", "NEIGHBORS <addr|ALL>",        "a remote node's neighbors, or every node's (gateway)" },
    { "RTC",  "RTC [addr|ALL]",                   "clock, wake cause and duty cycle" },
    { "TIME", "TIME SET [addr|ALL] <epoch|ISO>",  "set the clock (UTC)" },
    { "TIME", "TIME SYNC <addr|ALL>",             "push this board's clock to nodes (gateway)" },
    { "SLEEP","SLEEP [addr] <LIGHT|DEEP> <s>",    "sleep for s seconds (not on a gateway)" },
    { "SLEEP","SLEEP [addr] <mode> UNTIL <time>", "sleep until a UTC time (epoch/ISO/HH:MM)" },
    { "DUTY", "DUTY [addr] <mode> <on_s> <off_s>","repeat awake/asleep; add ALIGN for clock marks" },
    { "DUTY", "DUTY [addr|ALL] OFF",              "stop the duty cycle" },
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
            line[used] = '\0';      /* drop the partial name snprintf left behind */
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
    } else if (line_starts_with(line, "TELEM", &rest)) {
        telem_query(ctx, rest);
    } else if (line_is(line, "NODES") || line_is(line, "NODES?")) {
        list_nodes(ctx);
    } else if (line_is(line, "NEIGHBORS") || line_is(line, "NEIGHBORS?")) {
        neighbors_local(ctx);
    } else if (line_starts_with(line, "NEIGHBORS", &rest)) {
        neighbors_query(ctx, rest);
    } else if (line_is(line, "RTC") || line_is(line, "RTC?")) {
        rtc_command(ctx, NULL);
    } else if (line_starts_with(line, "RTC", &rest)) {
        rtc_command(ctx, rest);
    } else if (line_starts_with(line, "TIME SET", &rest)) {
        rtc_verb(ctx, "TIME", rest);
    } else if (line_starts_with(line, "TIME SYNC", &rest)) {
        rtc_verb(ctx, "SYNC", rest);
    } else if (line_starts_with(line, "SLEEP", &rest)) {
        rtc_verb(ctx, "SLEEP", rest);
    } else if (line_starts_with(line, "DUTY", &rest)) {
        rtc_verb(ctx, "DUTY", rest);
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
