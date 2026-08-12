#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lora_uart.h"
#include "crc.h"

static const char *TAG = "LORA_UART";

/* Forward declaration of CRC helper used in send path */
/* (crc8_calculate is declared in crc.h) */

/* ──────────── RX frame-assembler state ────────────
 *
 * UART delivers bytes in bursts; several nodes may transmit back-to-back,
 * so a single packet can arrive split across reads.  The old code read a
 * fixed 3-byte header block: if only 1-2 bytes had arrived, those bytes were
 * consumed and lost, and the next read slid out of frame → CRC errors and,
 * worse, data from one node mis-attributed to another.  Instead we keep a
 * small accumulation buffer and only consume bytes once they complete a
 * whole valid frame.  If CRC fails, we re-sync by dropping ONE byte and
 * trying again (handles back-to-back / boundary-corrupted frames).
 */
/* A node may transmit several frames back-to-back (e.g. REPORT sends data 0x01
 * immediately followed by ACK 0x03). The assembler must be able to hold more
 * than a single frame so a trailing frame is not dropped when the first one
 * completes. 20 comfortably holds a compact frame (≤9B) plus a legacy frame
 * (8B) back-to-back; legacy+legacy (16B) fits with room to spare. */
#define RX_BUF_MAX      20
#define RX_FRAME_LEGACY_LEN 8

static uint8_t  s_rx_buf[RX_BUF_MAX];
static int      s_rx_len = 0;

/* TX mutex — serialises downlink transmits across tasks (lora_rx/worker,
 * cmd_retry, MQTT callback) so two sends never interleave. */
static SemaphoreHandle_t s_tx_mutex = NULL;

/* Warning throttle — during an RF-noise / desync burst the resync loop would
 * otherwise print one WARN per byte (hundreds of lines). Emit at most one WARN
 * per RX_RESC_CHATTER_MS window; the rest are counted and summarized. */
#define RX_RESC_CHATTER_MS  1000
static uint32_t s_rx_resync_dropped = 0;
static uint64_t s_rx_resync_last_warn_ms = 0;

static void rx_resync_warn(void)
{
    uint64_t now = esp_timer_get_time() / 1000;
    if (now - s_rx_resync_last_warn_ms < RX_RESC_CHATTER_MS) {
        s_rx_resync_dropped++;
        return;
    }
    if (s_rx_resync_dropped > 0) {
        ESP_LOGW(TAG, "RX resync suppressed %lu drops in the last 1000 ms",
                 (unsigned long)s_rx_resync_dropped);
    }
    s_rx_resync_dropped = 0;
    s_rx_resync_last_warn_ms = now;
}

/* 0x05 (LORA_CMD_HEADER) is a DOWNLINK type. The E32 module reflects the
 * gateway's own downlink transmit back onto the shared UART RX line, so we
 * must be able to frame it (to consume the bytes) but will then discard it
 * silently — it is never a genuine node uplink. */
static bool rx_is_known_type(uint8_t type)
{
    return (type == PKT_TYPE_DATA || type == PKT_TYPE_HEARTBEAT ||
            type == PKT_TYPE_ACK || type == PKT_TYPE_ALARM ||
            type == PKT_TYPE_DATA_COMPACT || type == PKT_TYPE_CMD);
}

static int rx_frame_len(const uint8_t *buf)
{
    uint8_t type = buf[1];
    if (!rx_is_known_type(type)) {
        /* Unknown type byte — caller will resync */
        return -1;
    }
    if (type == PKT_TYPE_CMD) {
        return 6;            /* downlink command frame (node never sends it) */
    }
    if (type == PKT_TYPE_DATA_COMPACT) {
        uint8_t presence = buf[2];
        int n = __builtin_popcount((unsigned int)(presence & 0x1F));
        return 3 + n + 1;   /* header + payload + crc */
    }
    return RX_FRAME_LEGACY_LEN;         /* legacy fixed length */
}

/* Drop the oldest byte (resync helper) */
static void rx_drop_byte(void)
{
    if (s_rx_len <= 0) return;
    memmove(&s_rx_buf[0], &s_rx_buf[1], (size_t)(s_rx_len - 1));
    s_rx_len--;
}

/* Pull more bytes from the UART into the RX buffer (non-blocking-ish).
 * Returns true if at least one new byte was appended. */
static bool rx_fill(void)
{
    /* Only pull what fits the small accumulation buffer (one frame max).
     * Any trailing bytes stay in the UART FIFO and are read on the next
     * call — they are NOT lost. */
    int want = RX_BUF_MAX - s_rx_len;
    if (want <= 0) return false;

    int n = uart_read_bytes(LORA_UART_NUM, &s_rx_buf[s_rx_len],
                            want, pdMS_TO_TICKS(10));
    if (n <= 0) return false;
    s_rx_len += n;
    return true;
}

bool lora_read_packet(lora_uplink_packet_t *packet)
{
    if (packet == NULL) return false;

    /* Keep filling until we have at least a header, or nothing arrives. */
    while (s_rx_len < 3) {
        if (!rx_fill()) return false;
    }

    /* Attempt to parse a whole frame from the accumulated bytes. */
    int attempts = 0;
    while (attempts < RX_BUF_MAX) {    /* bounded resync loop */
        int frame_len = rx_frame_len(s_rx_buf);
        if (frame_len < 0) {
            /* Unknown type byte → resync by dropping one byte. */
            rx_resync_warn();
            rx_drop_byte();
            attempts++;
            continue;
        }

        if (s_rx_len < frame_len) {
            /* Partial frame — wait for the rest (do NOT consume partial). */
            if (!rx_fill()) return false;
            continue;
        }

        /* We have a full frame (s_rx_len >= frame_len). Verify CRC. */
        uint8_t pkt[RX_BUF_MAX];
        memcpy(pkt, s_rx_buf, (size_t)frame_len);

        uint8_t computed = crc8_calculate(pkt, (size_t)(frame_len - 1));
        if (computed != pkt[frame_len - 1]) {
            rx_resync_warn();
            rx_drop_byte();
            attempts++;
            continue;
        }

        /* Downlink echo (type 0x05 = LORA_CMD_HEADER): this is NOT an uplink
         * from a node — it is our own command/ACK that the E32 module reflected
         * back onto the shared RX line. Consume the 6 bytes and discard it
         * silently; then continue parsing in case a real uplink (from the node
         * that is currently awake) follows immediately behind it. */
        if (pkt[1] == PKT_TYPE_CMD) {
            int remain = s_rx_len - frame_len;
            if (remain > 0) {
                memmove(&s_rx_buf[0], &s_rx_buf[frame_len], (size_t)remain);
            }
            s_rx_len = remain;
            ESP_LOGD(TAG, "Discarded downlink echo (cmd=0x%02X)", pkt[2]);
            /* Keep looping: drop straight back into parsing (if any bytes are
             * still buffered) or wait for more (the node's real uplink). */
            if (s_rx_len >= 3) {
                attempts = 0;   /* restart the bounded resync with fresh buffer */
                continue;
            }
            return false;       /* buffer drained — caller will re-poll */
        }

        /* Valid frame — consume it and keep any trailing bytes. */
        memset(packet, 0, sizeof(lora_uplink_packet_t));
        packet->node_id = pkt[0];
        packet->type    = pkt[1];
        packet->presence = 0;

        if (pkt[1] == PKT_TYPE_DATA_COMPACT) {
            uint8_t presence = pkt[2];
            int off = 3;
            if (presence & PRESENCE_TEMPERATURE) {
                packet->temp = (int8_t)(pkt[off] - 40);
                off++;
            }
            if (presence & PRESENCE_HUMIDITY)    { packet->hum = pkt[off++]; }
            if (presence & PRESENCE_SOIL_MOIST)  { packet->soil = pkt[off++]; }
            if (presence & PRESENCE_BATTERY)     { packet->battery = pkt[off++]; }
            if (presence & PRESENCE_FLAGS)       { packet->flags = pkt[off++]; }
            packet->crc = pkt[frame_len - 1];
            packet->presence = presence;

            ESP_LOGD(TAG, "LoRa RX compact: Node=0x%02X Presence=0x%02X "
                     "Soil=%d Temp=%d Hum=%d Batt=%d Flags=0x%02X CRC=0x%02X",
                     packet->node_id, presence, packet->soil, packet->temp,
                     packet->hum, packet->battery, packet->flags, packet->crc);
        } else {
            packet->flags   = pkt[2];
            packet->soil    = pkt[3];
            /* Legacy: node encodes temp = temp°C + 40 (unsigned byte). */
            packet->temp    = (int8_t)(pkt[4] - 40);
            packet->hum     = pkt[5];
            packet->battery = pkt[6];
            packet->crc     = pkt[7];

            ESP_LOGD(TAG, "LoRa RX: Node=0x%02X Type=0x%02X Flags=0x%02X "
                     "Soil=%d Temp=%d Hum=%d Batt=%d CRC=0x%02X",
                     packet->node_id, pkt[1], packet->flags, packet->soil,
                     packet->temp, packet->hum, packet->battery, packet->crc);
        }

        /* Consume frame_len bytes from the accumulation buffer. */
        int remain = s_rx_len - frame_len;
        if (remain > 0) {
            memmove(&s_rx_buf[0], &s_rx_buf[frame_len], (size_t)remain);
        }
        s_rx_len = remain;

        return true;
    }

    /* Could not find a valid frame (many resyncs). Flush and restart. */
    ESP_LOGW(TAG, "RX failed to resync after %d bytes, flushing", s_rx_len);
    s_rx_len = 0;
    lora_flush_rx();
    return false;
}

esp_err_t lora_uart_init(void)
{
    ESP_LOGI(TAG, "Initializing LoRa UART on UART_NUM_2 (TX:%d, RX:%d)", 
             LORA_TX_GPIO, LORA_RX_GPIO);

    /* Configure UART parameters */
    uart_config_t uart_config = {
        .baud_rate  = LORA_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_param_config(LORA_UART_NUM, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART param config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Set UART pins */
    ret = uart_set_pin(LORA_UART_NUM, LORA_TX_GPIO, LORA_RX_GPIO, 
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART set pin failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Install UART driver */
    ret = uart_driver_install(LORA_UART_NUM, LORA_BUF_SIZE * 2, 
                              LORA_BUF_SIZE * 2, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Configure MD0, MD1 as outputs */
    gpio_config_t io_conf = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LORA_MD0_GPIO) | (1ULL << LORA_MD1_GPIO),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
    };
    ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config for MD0/MD1 failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Configure AUX as input */
    io_conf.intr_type    = GPIO_INTR_DISABLE;
    io_conf.mode         = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << LORA_AUX_GPIO);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en   = GPIO_PULLUP_DISABLE;
    ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config for AUX failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Set module to normal mode */
    lora_set_mode(LORA_MODE_NORMAL);

    /* Create the TX mutex (serialises downlink sends across tasks) */
    if (s_tx_mutex == NULL) {
        s_tx_mutex = xSemaphoreCreateMutex();
        configASSERT(s_tx_mutex != NULL);
    }

    /* Flush any stale data */
    uart_flush(LORA_UART_NUM);

    ESP_LOGI(TAG, "LoRa UART initialized successfully");
    return ESP_OK;
}

void lora_set_mode(lora_mode_t mode)
{
    switch (mode) {
        case LORA_MODE_NORMAL:
            gpio_set_level(LORA_MD0_GPIO, 0);
            gpio_set_level(LORA_MD1_GPIO, 0);
            break;
        case LORA_MODE_WAKEUP:
            gpio_set_level(LORA_MD0_GPIO, 1);
            gpio_set_level(LORA_MD1_GPIO, 0);
            break;
        case LORA_MODE_POWER_SAVING:
            gpio_set_level(LORA_MD0_GPIO, 0);
            gpio_set_level(LORA_MD1_GPIO, 1);
            break;
        case LORA_MODE_SLEEP:
            gpio_set_level(LORA_MD0_GPIO, 1);
            gpio_set_level(LORA_MD1_GPIO, 1);
            break;
    }
    ESP_LOGD(TAG, "LoRa mode set to %d (MD0=%d, MD1=%d)", 
             mode, gpio_get_level(LORA_MD0_GPIO), gpio_get_level(LORA_MD1_GPIO));
}

bool lora_wait_aux_ready(uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    while ((xTaskGetTickCount() - start) < timeout_ticks) {
        if (gpio_get_level(LORA_AUX_GPIO) == 1) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return false;
}

/*
 * Shared transmit path: wait for AUX, write the raw packet, wait for the
 * UART FIFO to drain and AUX to go high again (module ready after TX).
 */
static bool lora_transmit_packet(const uint8_t *buf, size_t len)
{
    /* Wait for AUX to indicate module ready */
    if (!lora_wait_aux_ready(LORA_TX_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "AUX not ready before send");
        return false;
    }

    /* Send the packet */
    int written = uart_write_bytes(LORA_UART_NUM, (const char *)buf, len);
    if (written != (int)len) {
        ESP_LOGE(TAG, "UART write failed: written %d, expected %d", written, (int)len);
        return false;
    }

    /* Wait for transmission to complete */
    ESP_ERROR_CHECK(uart_wait_tx_done(LORA_UART_NUM, pdMS_TO_TICKS(LORA_TX_TIMEOUT_MS)));

    /* Wait for AUX to go high again (module ready after transmit) */
    if (!lora_wait_aux_ready(LORA_TX_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "AUX not ready after send (module may still be busy)");
    }

    /* Clear any TX echo / stale bytes accumulated during transmit. The E32
     * module may reflect (loop) our own downlink onto the shared UART RX line,
     * corrupting the RX assembler for the next genuine uplink. Dropping those
     * bytes here guarantees frame sync starts clean for the node's reply. */
    if (uart_flush_input(LORA_UART_NUM) != ESP_OK) {
        ESP_LOGW(TAG, "UART RX flush after TX failed");
    }
    if (s_rx_len != 0) {
        s_rx_len = 0;   /* also clear bytes already staged in the assembler */
    }

    return true;
}

bool lora_send_command(uint8_t node_id, uint8_t command, uint8_t param1, uint8_t param2)
{
    /* Build the 6-byte downlink packet */
    uint8_t buf[LORA_DOWNLINK_SIZE];
    buf[0] = node_id;
    buf[1] = LORA_CMD_HEADER;      /* 0x05 */
    buf[2] = command;
    buf[3] = param1;
    buf[4] = param2;
    /* CRC is XOR of bytes 0-4, placed in byte 5 */
    buf[5] = crc8_calculate(buf, 5);

    /* Serialise TX against other tasks sending downlinks */
    if (s_tx_mutex != NULL &&
        xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "TX mutex timeout (cmd), dropping send to node 0x%02X",
                 node_id);
        return false;
    }

    bool ok = lora_transmit_packet(buf, LORA_DOWNLINK_SIZE);

    if (s_tx_mutex != NULL) {
        xSemaphoreGive(s_tx_mutex);
    }

    if (ok) {
        ESP_LOGD(TAG, "LoRa TX: Node=0x%02X Cmd=0x%02X P1=%d P2=%d CRC=0x%02X",
                 node_id, command, param1, param2, buf[5]);
    }

    return ok;
}

bool lora_send_ack(uint8_t node_id)
{
    /* Build the 6-byte downlink ACK packet: {node, 0x05, 0x09, 0, 0, crc} */
    uint8_t buf[LORA_DOWNLINK_SIZE];
    buf[0] = node_id;
    buf[1] = LORA_CMD_HEADER;      /* 0x05 */
    buf[2] = LORA_CMD_ACK;         /* 0x09 — no command, pure ACK */
    buf[3] = 0;
    buf[4] = 0;
    buf[5] = crc8_calculate(buf, 5);

    /* Serialise against other downlink transmitters */
    if (s_tx_mutex != NULL &&
        xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "TX mutex timeout, skipping ACK to node 0x%02X", node_id);
        return false;
    }

    bool ok = lora_transmit_packet(buf, LORA_DOWNLINK_SIZE);

    if (s_tx_mutex != NULL) {
        xSemaphoreGive(s_tx_mutex);
    }

    if (ok) {
        ESP_LOGD(TAG, "LoRa TX ACK: Node=0x%02X (no command) CRC=0x%02X",
                 node_id, buf[5]);
    }

    return ok;
}

void lora_flush_rx(void)
{
    uart_flush(LORA_UART_NUM);
}

void lora_uart_deinit(void)
{
    /* Gateway runs on direct power — keep module in normal mode (no power
     * saving needed). Tear down over a clean normal-mode state. */
    lora_set_mode(LORA_MODE_NORMAL);
    
    /* Uninstall UART driver */
    uart_driver_delete(LORA_UART_NUM);
    
    /* Reset GPIO pins */
    gpio_reset_pin(LORA_TX_GPIO);
    gpio_reset_pin(LORA_RX_GPIO);
    gpio_reset_pin(LORA_MD0_GPIO);
    gpio_reset_pin(LORA_MD1_GPIO);
    gpio_reset_pin(LORA_AUX_GPIO);
    
    ESP_LOGI(TAG, "LoRa UART deinitialized");
}
