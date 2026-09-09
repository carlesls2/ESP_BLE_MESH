/* uart2_cmd.c - Host commands over UART2 (dedicated external-host link) */

#include "uart2_cmd.h"

#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cmd_proto.h"

#define TAG "UART2_CMD"

#define UART2_CMD_PORT     UART_NUM_2
#define UART2_CMD_BAUD     115200
#define UART2_CMD_PIN_TX   17
#define UART2_CMD_PIN_RX   16
#define UART2_CMD_RX_BUF   1024
/* A real TX buffer (unlike UART0) so emit never blocks: bridge lines are
 * produced inside BLE-mesh callbacks, which must not stall on a slow host. */
#define UART2_CMD_TX_BUF   1024
#define UART2_CMD_TASK_STK 3072
#define UART2_CMD_TASK_PRI 10
#define UART2_CMD_READ_LEN 128

static cmd_proto_ctx_t s_ctx;
static bool s_ready = false;

void uart2_cmd_emit(const char *text)
{
    if (!s_ready || text == NULL) {
        return;
    }
    uart_write_bytes(UART2_CMD_PORT, text, strlen(text));
    uart_write_bytes(UART2_CMD_PORT, "\r\n", 2);
}

bool uart2_cmd_ready(void)
{
    return s_ready;
}

static void uart2_cmd_task(void *arg)
{
    uint8_t buf[UART2_CMD_READ_LEN];

    ESP_LOGI(TAG, "listening on UART%d (TX %d, RX %d)",
             UART2_CMD_PORT, UART2_CMD_PIN_TX, UART2_CMD_PIN_RX);

    for (;;) {
        int n = uart_read_bytes(UART2_CMD_PORT, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (n > 0) {
            cmd_proto_feed(&s_ctx, buf, (size_t)n);
        } else if (n < 0) {
            ESP_LOGE(TAG, "uart_read_bytes failed");
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

esp_err_t uart2_cmd_init(void)
{
    esp_err_t err;

    cmd_proto_ctx_init(&s_ctx, "uart2", uart2_cmd_emit);

    const uart_config_t cfg = {
        .baud_rate = UART2_CMD_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    err = uart_driver_install(UART2_CMD_PORT, UART2_CMD_RX_BUF, UART2_CMD_TX_BUF, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_param_config(UART2_CMD_PORT, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        uart_driver_delete(UART2_CMD_PORT);
        return err;
    }

    err = uart_set_pin(UART2_CMD_PORT, UART2_CMD_PIN_TX, UART2_CMD_PIN_RX,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        uart_driver_delete(UART2_CMD_PORT);
        return err;
    }

    if (xTaskCreate(uart2_cmd_task, "uart2_cmd", UART2_CMD_TASK_STK, NULL,
                    UART2_CMD_TASK_PRI, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create uart2_cmd task");
        uart_driver_delete(UART2_CMD_PORT);
        return ESP_ERR_NO_MEM;
    }

    s_ready = true;
    return ESP_OK;
}
