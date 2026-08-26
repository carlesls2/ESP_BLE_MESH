/* cmd_proto.h - Host command protocol shared by every transport
 *
 * The mode-switch code can arrive over UART or SPI. Both transports feed their
 * bytes into the same parser here, so the command set is defined once.
 *
 * Two wire forms are accepted, distinguished by the first byte of a frame:
 *
 *   ASCII, line terminated by CR and/or LF -- meant to be typed straight into
 *   the PlatformIO monitor:
 *       MODE GATEWAY
 *       MODE NODE
 *       MODE?
 *
 *   Binary, 2 bytes, for a host driving SPI:
 *       A5 01   become gateway
 *       A5 00   become node
 *       A5 3F   report current mode
 *
 * Replies go back through the transport's own reply hook so a UART command is
 * answered on UART and an SPI command on SPI.
 */

#ifndef _CMD_PROTO_H_
#define _CMD_PROTO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CMD_PROTO_MAGIC     0xA5
#define CMD_PROTO_OP_NODE    0x00
#define CMD_PROTO_OP_GATEWAY 0x01
#define CMD_PROTO_OP_QUERY   0x3F

/* Longest line we will buffer before giving up and resyncing. */
#define CMD_PROTO_MAX_LINE 64

/* How a transport sends a reply back to whoever issued the command. */
typedef void (*cmd_proto_reply_fn)(const char *text);

/* Per-transport parser state. Each transport owns one, so a partial line on
 * UART cannot be corrupted by an SPI frame arriving mid-way. */
typedef struct {
    char               line[CMD_PROTO_MAX_LINE];
    size_t             len;
    bool               overflow;   /* discarding until end of an over-long line */
    cmd_proto_reply_fn reply;
    const char        *tag;        /* transport name, for logging */
} cmd_proto_ctx_t;

void cmd_proto_ctx_init(cmd_proto_ctx_t *ctx, const char *tag, cmd_proto_reply_fn reply);

/* Feed received bytes. Complete commands are executed as they are recognised;
 * partial input is retained until the rest arrives. */
void cmd_proto_feed(cmd_proto_ctx_t *ctx, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* _CMD_PROTO_H_ */
