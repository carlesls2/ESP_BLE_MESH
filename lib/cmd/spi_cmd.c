/* spi_cmd.c - Mode-switch commands over SPI (slave) */

#include "spi_cmd.h"

#include <stdlib.h>
#include <string.h>

#include "driver/spi_slave.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "cmd_proto.h"

#define TAG "SPI_CMD"

/* VSPI / SPI3. HSPI is avoided because its default MISO is GPIO12 (MTDI), a
 * strapping pin that selects flash voltage at reset. These pins also stay clear
 * of LED_R (25), LED_G (26) and LED_B (2). */
#define SPI_CMD_HOST      SPI3_HOST
#define SPI_CMD_PIN_MOSI  23
#define SPI_CMD_PIN_MISO  19
#define SPI_CMD_PIN_SCLK  18
#define SPI_CMD_PIN_CS    5
#define SPI_CMD_DMA_CHAN  1

/* DMA transfers must be a multiple of 4 bytes and word-aligned. Every
 * transaction is this fixed size; the host pads short commands. */
#define SPI_CMD_BUF_LEN   64

#define SPI_CMD_TASK_STK  3072
#define SPI_CMD_TASK_PRI  10

/* Reply lines wait here until the host clocks a transaction for each. Deep
 * enough for the longest multi-line reply (HELP) plus a few async bridge
 * lines; on overflow the newest line is dropped with a warning. */
#define SPI_CMD_TX_QUEUE_LEN 16

static cmd_proto_ctx_t s_ctx;
static bool            s_ready = false;
static uint8_t        *s_rx_buf = NULL;
static uint8_t        *s_tx_buf = NULL;
static QueueHandle_t   s_tx_queue = NULL;

void spi_cmd_emit(const char *text)
{
    if (!s_ready || text == NULL) {
        return;
    }

    /* One queued frame per line; the host drains them one transaction at a
     * time and an all-zero frame means the queue is empty. Only spi_cmd_task
     * touches the DMA buffer, so a line can no longer be read torn. */
    uint8_t frame[SPI_CMD_BUF_LEN] = {0};
    strncpy((char *)frame, text, SPI_CMD_BUF_LEN - 1);
    if (xQueueSend(s_tx_queue, frame, 0) != pdTRUE) {
        ESP_LOGW(TAG, "tx queue full, dropped: %s", text);
    }
}

bool spi_cmd_ready(void)
{
    return s_ready;
}

static void spi_cmd_task(void *arg)
{
    ESP_LOGI(TAG, "listening on SPI%d (MOSI %d, MISO %d, SCLK %d, CS %d)",
             SPI_CMD_HOST + 1, SPI_CMD_PIN_MOSI, SPI_CMD_PIN_MISO,
             SPI_CMD_PIN_SCLK, SPI_CMD_PIN_CS);

    for (;;) {
        spi_slave_transaction_t t = {0};

        memset(s_rx_buf, 0, SPI_CMD_BUF_LEN);

        /* Stage the next queued reply line, or an all-zero frame that tells
         * the host there is nothing to read. */
        if (xQueueReceive(s_tx_queue, s_tx_buf, 0) != pdTRUE) {
            memset(s_tx_buf, 0, SPI_CMD_BUF_LEN);
        }

        t.length    = SPI_CMD_BUF_LEN * 8;
        t.tx_buffer = s_tx_buf;
        t.rx_buffer = s_rx_buf;

        /* Blocks until the host clocks a transaction. */
        esp_err_t err = spi_slave_transmit(SPI_CMD_HOST, &t, portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "spi_slave_transmit failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        size_t received = t.trans_len / 8;
        if (received == 0) {
            continue;
        }
        if (received > SPI_CMD_BUF_LEN) {
            received = SPI_CMD_BUF_LEN;
        }

        /* Every reply line a command produces is queued inside this call, so
         * they are all staged before the host's next poll is answered. */
        cmd_proto_feed(&s_ctx, s_rx_buf, received);
    }
}

esp_err_t spi_cmd_init(void)
{
    esp_err_t err;

    s_tx_queue = xQueueCreate(SPI_CMD_TX_QUEUE_LEN, SPI_CMD_BUF_LEN);
    if (s_tx_queue == NULL) {
        ESP_LOGE(TAG, "failed to create tx queue");
        return ESP_ERR_NO_MEM;
    }

    s_rx_buf = heap_caps_malloc(SPI_CMD_BUF_LEN, MALLOC_CAP_DMA);
    s_tx_buf = heap_caps_malloc(SPI_CMD_BUF_LEN, MALLOC_CAP_DMA);
    if (s_rx_buf == NULL || s_tx_buf == NULL) {
        ESP_LOGE(TAG, "failed to allocate DMA buffers");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    memset(s_rx_buf, 0, SPI_CMD_BUF_LEN);
    memset(s_tx_buf, 0, SPI_CMD_BUF_LEN);

    cmd_proto_ctx_init(&s_ctx, "spi", spi_cmd_emit);

    const spi_bus_config_t buscfg = {
        .mosi_io_num     = SPI_CMD_PIN_MOSI,
        .miso_io_num     = SPI_CMD_PIN_MISO,
        .sclk_io_num     = SPI_CMD_PIN_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = SPI_CMD_BUF_LEN,
    };

    const spi_slave_interface_config_t slvcfg = {
        .spics_io_num = SPI_CMD_PIN_CS,
        .flags        = 0,
        .queue_size   = 3,
        .mode         = 0,
    };

    err = spi_slave_initialize(SPI_CMD_HOST, &buscfg, &slvcfg, SPI_CMD_DMA_CHAN);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_slave_initialize failed: %s", esp_err_to_name(err));
        goto fail;
    }

    if (xTaskCreate(spi_cmd_task, "spi_cmd", SPI_CMD_TASK_STK, NULL,
                    SPI_CMD_TASK_PRI, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create spi_cmd task");
        spi_slave_free(SPI_CMD_HOST);
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    s_ready = true;
    return ESP_OK;

fail:
    free(s_rx_buf);
    free(s_tx_buf);
    s_rx_buf = NULL;
    s_tx_buf = NULL;
    vQueueDelete(s_tx_queue);
    s_tx_queue = NULL;
    return err;
}
