/* spi_cmd.c - Host commands over SPI (slave) */

#include "spi_cmd.h"

#include <stdlib.h>
#include <string.h>

#include "driver/spi_slave.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
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

/* No DMA. A frame fits the slave's own 64-byte buffer, and the DMA path is
 * where this chip's slave quirks live: a frame clocked outside a transaction
 * can leave its receive side corrupting every later frame until reboot (seen
 * here after a few hundred clean ones), and in mode 0 DMA launches MISO half a
 * clock early. */
#define SPI_CMD_DMA_CHAN  SPI_DMA_DISABLED

/* Every transaction is this fixed size; the host pads short commands. It is
 * also the most the slave moves without DMA. */
#define SPI_CMD_BUF_LEN   64

/* A line too long for one frame goes out in pieces, each frame led by one of
 * these instead of text. Neither can start a whole-line frame (printable ASCII)
 * or an empty one (0x00), and a host that predates them drops such frames as
 * unprintable rather than misreading them. */
#define SPI_FRAG_MORE     0x01
#define SPI_FRAG_END      0x02
#define SPI_FRAG_PAYLOAD  (SPI_CMD_BUF_LEN - 2)   /* less the lead byte and a NUL */

/* Transactions kept queued with the driver. One, deliberately: with two (and
 * DMA, at the time), the driver started the next one inside the completion
 * interrupt and about one frame in 150 went out as zeros or lost its first
 * byte, whatever the host's gap. One slot re-armed by spi_cmd_io_task, which
 * never blocks, soaked clean. */
#define SPI_CMD_SLOTS     1

#define SPI_CMD_IO_STK    2048
#define SPI_CMD_IO_PRI    12
#define SPI_CMD_EXEC_STK  3072
#define SPI_CMD_EXEC_PRI  10

/* Reply frames wait here until the host clocks a transaction for each. Deep
 * enough for HELP in fragments plus a burst of async bridge lines; a line that
 * does not fit is dropped whole with a warning. */
#define SPI_CMD_TX_QUEUE_LEN 48

/* Received command frames waiting for the exec task. */
#define SPI_CMD_RX_QUEUE_LEN 8

typedef struct {
    spi_slave_transaction_t t;
    uint8_t                *rx;
    uint8_t                *tx;
} spi_slot_t;

typedef struct {
    uint8_t len;
    uint8_t data[SPI_CMD_BUF_LEN];
} spi_rx_frame_t;

static cmd_proto_ctx_t   s_ctx;
static bool              s_ready = false;
static spi_slot_t        s_slots[SPI_CMD_SLOTS];
static QueueHandle_t     s_tx_queue = NULL;
static QueueHandle_t     s_rx_queue = NULL;
static SemaphoreHandle_t s_emit_lock = NULL;

void spi_cmd_emit(const char *text)
{
    if (!s_ready || text == NULL) {
        return;
    }

    size_t  len = strlen(text);
    size_t  frames = len < SPI_CMD_BUF_LEN
                   ? 1 : (len + SPI_FRAG_PAYLOAD - 1) / SPI_FRAG_PAYLOAD;
    uint8_t frame[SPI_CMD_BUF_LEN];

    /* Bridge lines come from BLE callbacks while replies come from the exec
     * task. Holding the lock across the whole line keeps its fragments
     * contiguous, and checking for room first queues a line whole or not at
     * all -- never a head without its END. Only spi_cmd_io_task touches the
     * frame buffers, so a queued frame cannot be read torn. */
    xSemaphoreTake(s_emit_lock, portMAX_DELAY);
    if (uxQueueSpacesAvailable(s_tx_queue) < frames) {
        xSemaphoreGive(s_emit_lock);
        ESP_LOGW(TAG, "tx queue full, dropped: %s", text);
        return;
    }
    if (frames == 1) {
        memset(frame, 0, sizeof(frame));
        memcpy(frame, text, len);
        xQueueSend(s_tx_queue, frame, 0);
    } else {
        for (size_t off = 0; off < len; off += SPI_FRAG_PAYLOAD) {
            size_t n = len - off < SPI_FRAG_PAYLOAD ? len - off : SPI_FRAG_PAYLOAD;
            memset(frame, 0, sizeof(frame));
            frame[0] = off + n < len ? SPI_FRAG_MORE : SPI_FRAG_END;
            memcpy(frame + 1, text + off, n);
            xQueueSend(s_tx_queue, frame, 0);
        }
    }
    xSemaphoreGive(s_emit_lock);
}

bool spi_cmd_ready(void)
{
    return s_ready;
}

/* Stages the next queued reply frame, or an all-zero one that tells the host
 * there is nothing to read, and hands the slot to the driver. */
static void arm(spi_slot_t *s)
{
    memset(s->rx, 0, SPI_CMD_BUF_LEN);
    if (xQueueReceive(s_tx_queue, s->tx, 0) != pdTRUE) {
        memset(s->tx, 0, SPI_CMD_BUF_LEN);
    }

    memset(&s->t, 0, sizeof(s->t));
    s->t.length    = SPI_CMD_BUF_LEN * 8;
    s->t.tx_buffer = s->tx;
    s->t.rx_buffer = s->rx;
    s->t.user      = s;

    esp_err_t err = spi_slave_queue_trans(SPI_CMD_HOST, &s->t, portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_slave_queue_trans failed: %s", esp_err_to_name(err));
    }
}

/* Only moves frames; commands run in spi_cmd_exec_task.
 *
 * Running a command can take tens of ms -- every reply line is also logged to
 * the console at 115200 baud, and more when another channel is logging too. A
 * host that clocks a frame while no transaction is queued desynchronises the
 * slave: the frame arrives torn, its newline is lost, and later commands run
 * together until the parser gives up with "command too long". Handing frames
 * off and re-arming the moment a transaction completes shrinks the unarmed gap
 * to the few microseconds that takes, far inside the host's inter-frame gap. */
static void spi_cmd_io_task(void *arg)
{
    ESP_LOGI(TAG, "listening on SPI%d (MOSI %d, MISO %d, SCLK %d, CS %d)",
             SPI_CMD_HOST + 1, SPI_CMD_PIN_MOSI, SPI_CMD_PIN_MISO,
             SPI_CMD_PIN_SCLK, SPI_CMD_PIN_CS);

    for (size_t i = 0; i < SPI_CMD_SLOTS; i++) {
        arm(&s_slots[i]);
    }

    for (;;) {
        spi_slave_transaction_t *done = NULL;
        esp_err_t err = spi_slave_get_trans_result(SPI_CMD_HOST, &done, portMAX_DELAY);
        if (err != ESP_OK || done == NULL) {
            ESP_LOGE(TAG, "spi_slave_get_trans_result failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        spi_slot_t *s = (spi_slot_t *)done->user;

        size_t bits = done->trans_len;  /* arm() clears the descriptor */
        size_t received = bits / 8;
        if (received > SPI_CMD_BUF_LEN) {
            received = SPI_CMD_BUF_LEN;
        }

        /* A poll is all zeros and carries nothing to run. */
        bool empty = true;
        for (size_t i = 0; i < received && empty; i++) {
            empty = s->rx[i] == 0;
        }
        if (!empty) {
            spi_rx_frame_t f = { .len = (uint8_t)received };
            memcpy(f.data, s->rx, received);
            if (xQueueSend(s_rx_queue, &f, 0) != pdTRUE) {
                ESP_LOGW(TAG, "rx queue full, dropped a command frame");
            }
        }

        arm(s);

        /* Logged only after re-arming, so the warning cannot open the gap it
         * reports. A short frame means the host clocked while nothing was
         * queued, or CS rose early; the line staged for it is lost. */
        if (bits != SPI_CMD_BUF_LEN * 8) {
            ESP_LOGW(TAG, "torn frame: %u of %u bits", (unsigned)bits,
                     SPI_CMD_BUF_LEN * 8);
        }
    }
}

/* Every reply line a command produces is queued inside cmd_proto_feed; the io
 * task stages them onto the host's following polls. */
static void spi_cmd_exec_task(void *arg)
{
    spi_rx_frame_t f;

    for (;;) {
        if (xQueueReceive(s_rx_queue, &f, portMAX_DELAY) == pdTRUE) {
            cmd_proto_feed(&s_ctx, f.data, f.len);
        }
    }
}

esp_err_t spi_cmd_init(void)
{
    esp_err_t    err = ESP_ERR_NO_MEM;
    TaskHandle_t exec = NULL;

    s_tx_queue  = xQueueCreate(SPI_CMD_TX_QUEUE_LEN, SPI_CMD_BUF_LEN);
    s_rx_queue  = xQueueCreate(SPI_CMD_RX_QUEUE_LEN, sizeof(spi_rx_frame_t));
    s_emit_lock = xSemaphoreCreateMutex();
    if (s_tx_queue == NULL || s_rx_queue == NULL || s_emit_lock == NULL) {
        ESP_LOGE(TAG, "failed to create queues");
        goto fail;
    }

    for (size_t i = 0; i < SPI_CMD_SLOTS; i++) {
        s_slots[i].rx = malloc(SPI_CMD_BUF_LEN);
        s_slots[i].tx = malloc(SPI_CMD_BUF_LEN);
        if (s_slots[i].rx == NULL || s_slots[i].tx == NULL) {
            ESP_LOGE(TAG, "failed to allocate frame buffers");
            goto fail;
        }
    }

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
        .queue_size   = SPI_CMD_SLOTS,
        .mode         = 0,
    };

    err = spi_slave_initialize(SPI_CMD_HOST, &buscfg, &slvcfg, SPI_CMD_DMA_CHAN);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_slave_initialize failed: %s", esp_err_to_name(err));
        goto fail;
    }

    if (xTaskCreate(spi_cmd_exec_task, "spi_exec", SPI_CMD_EXEC_STK, NULL,
                    SPI_CMD_EXEC_PRI, &exec) != pdPASS ||
        xTaskCreate(spi_cmd_io_task, "spi_cmd", SPI_CMD_IO_STK, NULL,
                    SPI_CMD_IO_PRI, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create spi tasks");
        if (exec != NULL) {
            vTaskDelete(exec);
        }
        spi_slave_free(SPI_CMD_HOST);
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    s_ready = true;
    return ESP_OK;

fail:
    for (size_t i = 0; i < SPI_CMD_SLOTS; i++) {
        free(s_slots[i].rx);
        free(s_slots[i].tx);
        s_slots[i].rx = NULL;
        s_slots[i].tx = NULL;
    }
    if (s_tx_queue != NULL) {
        vQueueDelete(s_tx_queue);
        s_tx_queue = NULL;
    }
    if (s_rx_queue != NULL) {
        vQueueDelete(s_rx_queue);
        s_rx_queue = NULL;
    }
    if (s_emit_lock != NULL) {
        vSemaphoreDelete(s_emit_lock);
        s_emit_lock = NULL;
    }
    return err;
}
