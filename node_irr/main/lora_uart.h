#ifndef LORA_UART_H
#define LORA_UART_H

#include <stdint.h>
#include <stddef.h>
#include "driver/uart.h"

/* LoRa UART Pin configuration (from AGENT.md final pins) */
#define LORA_UART_NUM          UART_NUM_2
#define LORA_UART_TX_GPIO      16
#define LORA_UART_RX_GPIO      17
#define LORA_PIN_MD0           22
#define LORA_PIN_MD1           21
#define LORA_PIN_AUX           19
#define LORA_UART_BAUD         9600

#define LORA_UART_RX_BUF_SIZE  256
#define LORA_UART_TX_BUF_SIZE  256

#define LORA_MODE_SWITCH_MS    40
#define LORA_AUX_TIMEOUT_MS    1000
#define LORA_CONFIG_DELAY_MS   50

/* LoRa operating modes */
typedef enum {
    LORA_MODE_NORMAL      = 0x00, /* MD0=0, MD1=0 */
    LORA_MODE_WAKEUP      = 0x01, /* MD0=1, MD1=0 */
    LORA_MODE_POWER_SAVING = 0x02, /* MD0=0, MD1=1 */
    LORA_MODE_CONFIG      = 0x03, /* MD0=1, MD1=1 */
} lora_mode_t;

/* SX1278 E32-433T module configuration structure */
typedef struct __attribute__((packed)) {
    uint8_t header;     /* 0xC0 (save on power-off) or 0xC2 (don't save) */
    uint8_t addr_high;  /* Module address high byte */
    uint8_t addr_low;   /* Module address low byte */
    uint8_t speed;      /* UART baud, parity, wireless air speed */
    uint8_t channel;    /* RF channel (frequency = 410M + channel * 1M) */
    uint8_t options;    /* Transmission mode, IO drive, wakeup time, FEC, power */
} e32_config_t;

/**
 * @brief Initialize LoRa UART module and control pins
 */
void lora_uart_init(void);

/**
 * @brief Set LoRa module mode (MD0/MD1 level setting)
 */
void lora_set_mode(lora_mode_t mode);

/**
 * @brief Wait for AUX pin to become HIGH (idle/ready)
 * @return 0 on success, -1 on timeout
 */
int lora_wait_aux(int timeout_ms);

/**
 * @brief Send raw data bytes via LoRa UART
 * @return Number of bytes written, or -1 on error
 */
int lora_send(const uint8_t *data, size_t len);

/**
 * @brief Read available data bytes from LoRa UART
 * @return Number of bytes read, or -1 on error
 */
int lora_receive(uint8_t *data, size_t buf_size);

/**
 * @brief Flush UART RX buffer
 */
void lora_flush(void);

/**
 * @brief Write configuration to LoRa module (permanent or temporary)
 * @return 0 on success, -1 on error
 */
int lora_configure_module(const e32_config_t *config);

/**
 * @brief Read configuration from LoRa module
 * @return 0 on success, -1 on error
 */
int lora_read_configuration(e32_config_t *config);

/**
 * @brief Put LoRa module into sleep mode to conserve power
 */
void lora_sleep(void);

/**
 * @brief Wake LoRa module and return to normal mode
 */
void lora_wake(void);

#endif /* LORA_UART_H */
