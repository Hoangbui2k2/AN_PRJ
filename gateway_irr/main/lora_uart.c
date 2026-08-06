#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "lora_uart.h"
#include "crc.h"

static const char *TAG = "LORA_UART";

/* Forward declaration of CRC helper used in send path */
/* (crc8_calculate is declared in crc.h) */

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

bool lora_read_packet(lora_uplink_packet_t *packet)
{
    /* Read minimum header: node_id(1) + type(1) + third_byte(1) */
    uint8_t header[3];
    int len = uart_read_bytes(LORA_UART_NUM, header, 3, pdMS_TO_TICKS(50));
    if (len != 3) {
        return false;
    }

    uint8_t type = header[1];

    /* Validate packet type */
    if (type != PKT_TYPE_DATA && type != PKT_TYPE_HEARTBEAT &&
        type != PKT_TYPE_ACK && type != PKT_TYPE_ALARM &&
        type != PKT_TYPE_DATA_COMPACT) {
        ESP_LOGW(TAG, "Unknown packet type 0x%02X from node 0x%02X, discarding",
                 type, header[0]);
        lora_flush_rx();
        return false;
    }

    /* Zero out the packet struct */
    memset(packet, 0, sizeof(lora_uplink_packet_t));
    packet->node_id = header[0];
    packet->type    = type;
    packet->presence = 0; /* 0 = legacy, non-zero = compact presence mask */

    if (type == PKT_TYPE_DATA_COMPACT) {
        /* ── Compact variable-length packet (4-9 bytes) ── */
        uint8_t presence = header[2];
        int payload_count = __builtin_popcount((unsigned int)(presence & 0x1F));

        if (payload_count < 0 || payload_count > 5) {
            ESP_LOGW(TAG, "Invalid compact payload count %d (presence 0x%02X)",
                     payload_count, presence);
            lora_flush_rx();
            return false;
        }

        /* Read payload (variable length) + CRC (1 byte) */
        int remaining = payload_count + 1;
        uint8_t payload[6];
        int got = uart_read_bytes(LORA_UART_NUM, payload, remaining, pdMS_TO_TICKS(30));
        if (got != remaining) {
            ESP_LOGW(TAG, "Short compact packet: got %d, expected %d", got, remaining);
            lora_flush_rx();
            return false;
        }

        packet->presence = presence;

        /* Parse payload fields in LSB-first order: temp → hum → soil → battery → flags */
        int off = 0;
        if (presence & PRESENCE_TEMPERATURE) {
            /* Wire: stored = temp_C + 40  →  actual = byte - 40 */
            packet->temp = (int8_t)(payload[off] - 40);
            off++;
        }
        if (presence & PRESENCE_HUMIDITY) {
            packet->hum = payload[off++];
        }
        if (presence & PRESENCE_SOIL_MOIST) {
            packet->soil = payload[off++];
        }
        if (presence & PRESENCE_BATTERY) {
            packet->battery = payload[off++];
        }
        if (presence & PRESENCE_FLAGS) {
            packet->flags = payload[off++];
        }

        /* CRC is the last byte */
        packet->crc = payload[payload_count];

        /* Verify CRC: XOR of header(3) + payload (excluding CRC byte) */
        uint8_t verify_buf[9];
        verify_buf[0] = header[0];
        verify_buf[1] = header[1];
        verify_buf[2] = header[2];
        memcpy(&verify_buf[3], payload, payload_count);
        uint8_t computed = crc8_calculate(verify_buf, 3 + payload_count);
        if (computed != packet->crc) {
            ESP_LOGW(TAG, "CRC mismatch on compact packet: computed 0x%02X, received 0x%02X",
                     computed, packet->crc);
            return false;
        }

        ESP_LOGD(TAG, "LoRa RX compact: Node=0x%02X Presence=0x%02X "
                 "Soil=%d Temp=%d Hum=%d Batt=%d Flags=0x%02X CRC=0x%02X",
                 packet->node_id, presence,
                 packet->soil, packet->temp, packet->hum, packet->battery,
                 packet->flags, packet->crc);

        return true;
    } else {
        /* ── Legacy 8-byte packet (0x01, 0x02, 0x03, 0x04) ── */
        uint8_t rest[5];
        int got = uart_read_bytes(LORA_UART_NUM, rest, 5, pdMS_TO_TICKS(30));
        if (got != 5) {
            return false;
        }

        packet->flags   = header[2];
        packet->soil    = rest[0];
        /* Legacy: node encodes temp = temp°C + 40 (unsigned byte). Decode back
         * to °C (-40..+85) to stay consistent with the compact 0x06 format. */
        packet->temp    = (int8_t)(rest[1] - 40);
        packet->hum     = rest[2];
        packet->battery = rest[3];
        packet->crc     = rest[4];

        /* Verify CRC: XOR of all 8 bytes before the CRC byte */
        uint8_t verify_buf[8] = {
            header[0], header[1], header[2],
            rest[0], rest[1], rest[2], rest[3], rest[4]
        };
        uint8_t computed = crc8_calculate(verify_buf, 7);
        if (computed != verify_buf[7]) {
            ESP_LOGW(TAG, "CRC mismatch: computed 0x%02X, received 0x%02X "
                     "(pkt: %02X %02X %02X %02X %02X %02X %02X %02X)",
                     computed, verify_buf[7],
                     verify_buf[0], verify_buf[1], verify_buf[2],
                     verify_buf[3], verify_buf[4], verify_buf[5],
                     verify_buf[6], verify_buf[7]);
            return false;
        }

        ESP_LOGD(TAG, "LoRa RX: Node=0x%02X Type=0x%02X Flags=0x%02X "
                 "Soil=%d Temp=%d Hum=%d Batt=%d CRC=0x%02X",
                 packet->node_id, type, packet->flags,
                 packet->soil, packet->temp, packet->hum, packet->battery, packet->crc);

        return true;
    }
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

    if (!lora_transmit_packet(buf, LORA_DOWNLINK_SIZE)) {
        return false;
    }

    ESP_LOGD(TAG, "LoRa TX: Node=0x%02X Cmd=0x%02X P1=%d P2=%d CRC=0x%02X",
             node_id, command, param1, param2, buf[5]);

    return true;
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

    if (!lora_transmit_packet(buf, LORA_DOWNLINK_SIZE)) {
        return false;
    }

    ESP_LOGD(TAG, "LoRa TX ACK: Node=0x%02X (no command) CRC=0x%02X", node_id, buf[5]);

    return true;
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
