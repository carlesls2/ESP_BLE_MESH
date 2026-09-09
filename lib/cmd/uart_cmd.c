/* uart_cmd.c - Mode-switch commands over UART0 */

#include "uart_cmd.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cmd_proto.h"

#define TAG "UART_CMD"

/* UART0 is the console. Installing the driver on it lets us read RX without
 * disturbing the log output already going out on TX, which is why commands can
 * be typed straight into `pio device monitor`. The cost is that replies and log
 * lines share the terminal. */
#define UART_CMD_PORT     UART_NUM_0
#define UART_CMD_BAUD     115200
#define UART_CMD_RX_BUF   1024
#define UART_CMD_TX_BUF   0      /* 0 = blocking writes, no driver TX buffer */
#define UART_CMD_TASK_STK 3072
#define UART_CMD_TASK_PRI 10
#define UART_CMD_READ_LEN 128

static cmd_proto_ctx_t s_ctx;
static bool s_ready = false;

void uart_cmd_emit(const char *text)
{
    if (!s_ready || text == NULL) {
        return;
    }

    /* Written through stdout in ONE call, rather than via uart_write_bytes().
     *
     * ESP_LOG reaches UART0 through stdout's vprintf; uart_write_bytes() is a
     * separate path to the same peripheral. With two independent writers a log
     * line lands *inside* a reply -- observed in the wild as
     *
     *     HELP NODES        list nodes...
     *     HI (11376) EXAMPLE: BLE Mesh running as GATEWAY
     *     ELP HELP [command] ...
     *
     * which corrupts both lines and cannot be repaired by any filtering at the
     * host end. Going through stdout makes this share the FILE lock that
     * vprintf takes, so emits and log lines interleave only at line
     * boundaries, which the host filter does handle. */
    printf("%s\r\n", text);
    fflush(stdout);
}

bool uart_cmd_ready(void)
{
    return s_ready;
}

static void uart_cmd_task(void *arg)
{
    uint8_t buf[UART_CMD_READ_LEN];

    ESP_LOGI(TAG, "listening on UART%d for MODE commands", UART_CMD_PORT);

    for (;;) {
        int n = uart_read_bytes(UART_CMD_PORT, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (n > 0) {
            cmd_proto_feed(&s_ctx, buf, (size_t)n);
        } else if (n < 0) {
            ESP_LOGE(TAG, "uart_read_bytes failed");
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

esp_err_t uart_cmd_init(void)
{
    esp_err_t err;

    cmd_proto_ctx_init(&s_ctx, "uart", uart_cmd_emit);

    const uart_config_t cfg = {
        .baud_rate = UART_CMD_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    err = uart_driver_install(UART_CMD_PORT, UART_CMD_RX_BUF, UART_CMD_TX_BUF, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_param_config(UART_CMD_PORT, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        uart_driver_delete(UART_CMD_PORT);
        return err;
    }

    /* Console pins are already routed by the bootloader; no uart_set_pin(). */

    if (xTaskCreate(uart_cmd_task, "uart_cmd", UART_CMD_TASK_STK, NULL,
                    UART_CMD_TASK_PRI, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create uart_cmd task");
        uart_driver_delete(UART_CMD_PORT);
        return ESP_ERR_NO_MEM;
    }

    s_ready = true;
    return ESP_OK;
}
