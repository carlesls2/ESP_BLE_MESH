/* uart_cmd.h - Mode-switch commands over UART0
 *
 * UART0 is also the console, so commands typed into the PlatformIO monitor land
 * here while ESP_LOG output continues to go out on the same TX line. That
 * interleaving is expected -- see the note in uart_cmd.c.
 */

#ifndef _UART_CMD_H_
#define _UART_CMD_H_

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t uart_cmd_init(void);

/* Emits a line to the host. Used by the mesh->host bridge in gateway mode. */
void uart_cmd_emit(const char *text);

bool uart_cmd_ready(void);

#ifdef __cplusplus
}
#endif
#endif /* _UART_CMD_H_ */
