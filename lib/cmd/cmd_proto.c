/* cmd_proto.c - Host command protocol shared by every transport */

#include "cmd_proto.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"

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

    if (line_is(line, "MODE GATEWAY")) {
        apply_mode(ctx, DEVICE_MODE_GATEWAY);
    } else if (line_is(line, "MODE NODE")) {
        apply_mode(ctx, DEVICE_MODE_NODE);
    } else if (line_is(line, "MODE?") || line_is(line, "MODE")) {
        report_mode(ctx);
    } else {
        reply(ctx, "ERR unknown command (try MODE GATEWAY | MODE NODE | MODE?)");
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
