/* spi_cmd.h - Mode-switch commands over SPI (slave)
 *
 * The ESP32 is the slave; an external host clocks command frames in. Uses
 * VSPI / SPI3 rather than HSPI because HSPI's default MISO is GPIO12 (MTDI), a
 * strapping pin that selects flash voltage at reset -- a host driving it while
 * the board comes up can leave it unbootable.
 *
 * Default pins: MOSI 23, MISO 19, SCLK 18, CS 5.
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
