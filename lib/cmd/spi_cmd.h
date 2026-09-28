/* spi_cmd.h - Host commands over SPI (slave)
 *
 * The ESP32 is the slave; an external host clocks command frames in. Uses
 * VSPI / SPI3 rather than HSPI because HSPI's default MISO is GPIO12 (MTDI), a
 * strapping pin that selects flash voltage at reset -- a host driving it while
 * the board comes up can leave it unbootable.
 *
 * Default pins: MOSI 23, MISO 19, SCLK 18, CS 5.
 *
 * Framing: every transaction is exactly 64 bytes. The host sends a
 * newline-terminated ASCII command zero-padded to the frame; the slave returns
 * one queued reply frame per transaction. A frame whose first byte is 0x00
 * carries nothing -- the host polls with zero-filled frames until it sees that.
 * A line of up to 63 chars travels as one NUL-terminated frame. A longer one is
 * split into frames led by 0x01 (more follows) and a last one led by 0x02, each
 * carrying up to 62 chars, which the host concatenates. Reply frames wait in a
 * queue, so multi-line replies (HELP, ID?) and async bridge lines all arrive.
 *
 * Commands run in their own task while a second one only moves frames and
 * re-arms the slave within microseconds of each transaction, so the host never
 * clocks into an unarmed slave. A reply starts a frame or two after its
 * command: poll until the queue has been quiet for a while rather than
 * stopping at the first empty frame.
 */

#ifndef _SPI_CMD_H_
#define _SPI_CMD_H_

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t spi_cmd_init(void);

/* Queues a line to be returned to the host on the next transaction. Used by the
 * mesh->host bridge in gateway mode. */
void spi_cmd_emit(const char *text);

bool spi_cmd_ready(void);

#ifdef __cplusplus
}
#endif
#endif /* _SPI_CMD_H_ */
