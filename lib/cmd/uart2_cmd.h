/* uart2_cmd.h - Host commands over UART2 (dedicated external-host link)
 *
 * Unlike UART0, this channel is not shared with the console: replies and bridge
 * lines go out with no ESP_LOG noise, which makes it the link a Raspberry Pi
 * (or any other host) should parse. Pins: TX GPIO17, RX GPIO16. Note that
 * GPIO16/17 are taken by PSRAM on WROVER modules -- fine on WROOM boards.
 */

#ifndef _UART2_CMD_H_
#define _UART2_CMD_H_

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t uart2_cmd_init(void);

/* Emits a line to the host. Used by the mesh->host bridge in gateway mode. */
void uart2_cmd_emit(const char *text);

bool uart2_cmd_ready(void);

#ifdef __cplusplus
}
#endif
#endif /* _UART2_CMD_H_ */
