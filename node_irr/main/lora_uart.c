#include "lora_uart.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "LORA_UART";

/* AUX debounce delay (module needs ~2ms after mode change) */
#define AUX_DEBOUNCE_MS 3

void lora_uart_init(void)
{
    ESP_LOGI(TAG, "Initializing LoRa UART (UART2)");

    /* Configure control GPIOs */
    const gpio_config_t ctrl_pins = {
        .pin_bit_mask = (1ULL << LORA_PIN_MD0) | (1ULL << LORA_PIN_MD1),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&ctrl_pins);

    /* Configure AUX input */
    const gpio_config_t aux_pin = {
        .pin_bit_mask = (1ULL << LORA_PIN_AUX),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&aux_pin);

    /* Start in normal mode */
    gpio_set_level(LORA_PIN_MD0, 0);
    gpio_set_level(LORA_PIN_MD1, 0);
    vTaskDelay(pdMS_TO_TICKS(LORA_MODE_SWITCH_MS));

    /* Configure UART2 */
    const uart_config_t uart_cfg = {
        .baud_rate = LORA_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    ESP_ERROR_CHECK(uart_param_config(LORA_UART_NUM, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(LORA_UART_NUM, LORA_UART_TX_GPIO, LORA_UART_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(LORA_UART_NUM, LORA_UART_RX_BUF_SIZE,
                                        LORA_UART_TX_BUF_SIZE, 0, NULL, 0));

    /* Wait for AUX to indicate module ready */
    if (lora_wait_aux(LORA_AUX_TIMEOUT_MS) != 0) {
        ESP_LOGW(TAG, "LoRa module not responding on AUX after init");
    } else {
        ESP_LOGI(TAG, "LoRa module ready");
    }
}

void lora_set_mode(lora_mode_t mode)
{
    uint8_t md0 = (mode >> 0) & 1;
    uint8_t md1 = (mode >> 1) & 1;

    gpio_set_level(LORA_PIN_MD0, md0);
    gpio_set_level(LORA_PIN_MD1, md1);

    /* Wait for mode switch settling */
    vTaskDelay(pdMS_TO_TICKS(LORA_MODE_SWITCH_MS));

    /* Wait for AUX to confirm */
    lora_wait_aux(LORA_AUX_TIMEOUT_MS);

    ESP_LOGD(TAG, "Mode set: MD0=%d, MD1=%d (0x%02X)", md0, md1, mode);
}

int lora_wait_aux(int timeout_ms)
{
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        if (gpio_get_level(LORA_PIN_AUX) == 1) {
            return 0;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
        elapsed++;
    }
    ESP_LOGW(TAG, "AUX timeout after %d ms", timeout_ms);
    return -1;
}

/**
 * @brief Wait for AUX pin to complete a LOW→HIGH transition.
 *
 * After data is written to the E32 module, AUX goes LOW (busy transmitting)
 * and then returns HIGH (idle).  This helper waits for the full cycle so the
 * caller knows the wireless transmission has actually finished.
 *
 * @param busy_timeout_ms  Max time to wait for AUX to go LOW (transmit start)
 * @param idle_timeout_ms  Max time to wait for AUX to go HIGH (transmit done)
 * @return 0 on success, -1 if either timeout expires
 */
static int lora_wait_tx_complete(int busy_timeout_ms, int idle_timeout_ms)
{
    int elapsed;

    /* 1) Wait for AUX → LOW  — module has started transmitting */
    elapsed = 0;
    while (elapsed < busy_timeout_ms) {
        if (gpio_get_level(LORA_PIN_AUX) == 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
        elapsed++;
    }
    if (elapsed >= busy_timeout_ms) {
        ESP_LOGW(TAG, "AUX did not go LOW (TX start timeout %d ms)", busy_timeout_ms);
        /* Not necessarily fatal — the module may have finished already if the
         * packet was very short.  Proceed to the idle wait. */
    }

    /* 2) Wait for AUX → HIGH — module has finished transmitting */
    elapsed = 0;
    while (elapsed < idle_timeout_ms) {
        if (gpio_get_level(LORA_PIN_AUX) == 1) {
            return 0;              /* success */
        }
        vTaskDelay(pdMS_TO_TICKS(1));
        elapsed++;
    }

    ESP_LOGW(TAG, "AUX TX completion timeout after %d ms", idle_timeout_ms);
    return -1;
}

int lora_send(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return -1;
    }

    /* Ensure in normal mode */
    lora_set_mode(LORA_MODE_NORMAL);

    /* Wait for AUX ready before sending */
    if (lora_wait_aux(LORA_AUX_TIMEOUT_MS) != 0) {
        ESP_LOGE(TAG, "AUX not ready before send");
        return -1;
    }

    /* Write data to UART */
    int written = uart_write_bytes(LORA_UART_NUM, (const char *)data, len);
    if (written < 0 || (size_t)written != len) {
        ESP_LOGE(TAG, "UART write failed: wrote %d/%d bytes", written, len);
        return -1;
    }

    /* Wait for transmission to complete (AUX goes LOW then HIGH) */
    if (lora_wait_tx_complete(LORA_AUX_TIMEOUT_MS, LORA_AUX_TIMEOUT_MS) != 0) {
        ESP_LOGW(TAG, "LoRa TX completion uncertain");
    }

    ESP_LOGD(TAG, "Sent %d bytes", written);
    return written;
}

int lora_receive(uint8_t *data, size_t buf_size)
{
    if (data == NULL || buf_size == 0) {
        return -1;
    }

    size_t available = 0;
    ESP_ERROR_CHECK(uart_get_buffered_data_len(LORA_UART_NUM, &available));

    if (available == 0) {
        return 0;
    }

    if (available > buf_size) {
        ESP_LOGW(TAG, "RX buffer overflow: %d available, %d capacity", available, buf_size);
        available = buf_size;
    }

    int read_bytes = uart_read_bytes(LORA_UART_NUM, data, available, pdMS_TO_TICKS(10));
    if (read_bytes < 0) {
        return -1;
    }

    ESP_LOGD(TAG, "Received %d bytes", read_bytes);
    return read_bytes;
}

void lora_flush(void)
{
    uart_flush(LORA_UART_NUM);
    ESP_LOGD(TAG, "RX buffer flushed");
}

int lora_configure_module(const e32_config_t *config)
{
    if (config == NULL) {
        return -1;
    }

    /* Switch to config mode (MD0=0, MD1=1) */
    lora_set_mode(LORA_MODE_CONFIG);
    vTaskDelay(pdMS_TO_TICKS(LORA_CONFIG_DELAY_MS));

    /* Wait for AUX ready */
    if (lora_wait_aux(LORA_AUX_TIMEOUT_MS) != 0) {
        ESP_LOGE(TAG, "AUX timeout entering config mode");
        lora_set_mode(LORA_MODE_NORMAL);
        return -1;
    }

    /* Send configuration bytes */
    int written = uart_write_bytes(LORA_UART_NUM, (const char *)config, sizeof(e32_config_t));
    if (written != sizeof(e32_config_t)) {
        ESP_LOGE(TAG, "Failed to write config to module");
        lora_set_mode(LORA_MODE_NORMAL);
        return -1;
    }

    /* Wait for save */
    vTaskDelay(pdMS_TO_TICKS(LORA_CONFIG_DELAY_MS));
    lora_wait_aux(LORA_AUX_TIMEOUT_MS);

    /* Return to normal mode */
    lora_set_mode(LORA_MODE_NORMAL);
    vTaskDelay(pdMS_TO_TICKS(LORA_CONFIG_DELAY_MS));

    ESP_LOGI(TAG, "Module configured: addr=0x%02X%02X, ch=0x%02X",
             config->addr_high, config->addr_low, config->channel);
    return 0;
}

int lora_read_configuration(e32_config_t *config)
{
    if (config == NULL) {
        return -1;
    }

    uint8_t cmd[3] = {0xC1, 0x00, 0x00};

    /* Switch to config mode */
    lora_set_mode(LORA_MODE_CONFIG);
    vTaskDelay(pdMS_TO_TICKS(LORA_CONFIG_DELAY_MS));

    if (lora_wait_aux(LORA_AUX_TIMEOUT_MS) != 0) {
        ESP_LOGE(TAG, "AUX timeout entering config mode for read");
        lora_set_mode(LORA_MODE_NORMAL);
        return -1;
    }

    /* Send read command */
    lora_flush();
    uart_write_bytes(LORA_UART_NUM, (const char *)cmd, 3);

    /* Wait for response */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Read response */
    int read_bytes = uart_read_bytes(LORA_UART_NUM, (uint8_t *)config,
                                     sizeof(e32_config_t), pdMS_TO_TICKS(100));
    if (read_bytes != sizeof(e32_config_t)) {
        ESP_LOGE(TAG, "Failed to read config: got %d bytes", read_bytes);
        lora_set_mode(LORA_MODE_NORMAL);
        memset(config, 0, sizeof(e32_config_t));
        return -1;
    }

    /* Return to normal mode */
    lora_set_mode(LORA_MODE_NORMAL);
    vTaskDelay(pdMS_TO_TICKS(LORA_CONFIG_DELAY_MS));

    ESP_LOGI(TAG, "Module config read: header=0x%02X, addr=0x%02X%02X, speed=0x%02X, ch=0x%02X, opts=0x%02X",
             config->header, config->addr_high, config->addr_low,
             config->speed, config->channel, config->options);
    return 0;
}

void lora_sleep(void)
{
    /* Enter deep sleep mode: MD0=1, MD1=1 */
    gpio_set_level(LORA_PIN_MD0, 1);
    gpio_set_level(LORA_PIN_MD1, 1);
    vTaskDelay(pdMS_TO_TICKS(LORA_MODE_SWITCH_MS));
    ESP_LOGI(TAG, "LoRa module in sleep mode");
}

void lora_wake(void)
{
    /* Wake: MD0=0, MD1=0 (normal mode) */
    gpio_set_level(LORA_PIN_MD0, 0);
    gpio_set_level(LORA_PIN_MD1, 0);
    vTaskDelay(pdMS_TO_TICKS(LORA_MODE_SWITCH_MS));

    if (lora_wait_aux(LORA_AUX_TIMEOUT_MS) != 0) {
        ESP_LOGW(TAG, "AUX not ready after wake");
    } else {
        ESP_LOGI(TAG, "LoRa module woken");
    }

    lora_flush();
}
