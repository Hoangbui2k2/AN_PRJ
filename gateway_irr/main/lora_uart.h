#ifndef LORA_UART_H
#define LORA_UART_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* GPIO Pin Definitions */
#define LORA_UART_NUM      UART_NUM_2
#define LORA_TX_GPIO       16
#define LORA_RX_GPIO       17
#define LORA_MD0_GPIO      22
#define LORA_MD1_GPIO      21
#define LORA_AUX_GPIO      19

/* UART Configuration */
#define LORA_BAUD_RATE     9600
#define LORA_BUF_SIZE      256
#define LORA_TX_TIMEOUT_MS 100

/* Packet Definitions */
#define LORA_UPLINK_SIZE   8   /* Uplink packet size (Node -> Gateway) */
#define LORA_DOWNLINK_SIZE 6   /* Downlink packet size (Gateway -> Node) */
#define LORA_CMD_HEADER    0x05 /* Command header byte for downlink */

/* LoRa Module Mode Control (EBYTE E32 standard: M0/M1 = MD0/MD1) */
typedef enum {
    LORA_MODE_NORMAL       = 0,  /* MD0=0, MD1=0 — normal operation */
    LORA_MODE_WAKEUP       = 1,  /* MD0=1, MD1=0 — wake-up / WOR */
    LORA_MODE_POWER_SAVING = 2,  /* MD0=0, MD1=1 — power saving */
    LORA_MODE_SLEEP        = 3,  /* MD0=1, MD1=1 — sleep, used for config 0xC0/0xC1 */
} lora_mode_t;

/* Uplink Packet Structure (8 bytes legacy + metadata) */
typedef struct __attribute__((packed)) {
    uint8_t node_id;     /* Byte 0: Node ID */
    uint8_t type;        /* Byte 1: Packet type */
    uint8_t flags;       /* Byte 2: Flags (legacy) / PRESENCE bitmask (compact type 0x06) */
    uint8_t soil;        /* Byte 3: Soil moisture */
    int8_t  temp;        /* Byte 4: Temperature (signed) */
    uint8_t hum;         /* Byte 5: Humidity */
    uint8_t battery;     /* Byte 6: Battery level */
    uint8_t crc;         /* Byte 7: CRC8 checksum */
    uint8_t presence;    /* For type 0x06: which fields were present (0 = legacy type) */
} lora_uplink_packet_t;

/* Packet Types */
#define PKT_TYPE_DATA           0x01  /* Sensor data (soil/temp/hum/battery) — 8 bytes */
#define PKT_TYPE_HEARTBEAT      0x02  /* Node heartbeat — just alive signal */
#define PKT_TYPE_ACK            0x03  /* ACK for received commands */
#define PKT_TYPE_ALARM          0x04  /* Sensor alarm */
#define PKT_TYPE_CMD            0x05  /* Gateway → node downlink command (6 bytes) */
#define PKT_TYPE_DATA_COMPACT   0x06  /* Compact variable-length data (4-9 bytes) */

/* Uplink Flags (byte 2 in legacy / flags field in compact payload) */
#define FLAG_PUMP_ON                0x01  /* Bit 0: Pump is running */
#define FLAG_THRESHOLD_EXCEEDED     0x02  /* Bit 1: Soil outside absolute threshold band */
#define FLAG_GW_LOST                0x04  /* Bit 2: Gateway lost signal detected by node */
#define FLAG_SENSOR_OK              0x08  /* Bit 3: Sensor health check passed */

/* Presence bitmask for DATA_COMPACT (byte 2 = presence, not flags) */
#define PRESENCE_TEMPERATURE   0x01  /* bit0: temperature (1 byte, offset+40 → °C) */
#define PRESENCE_HUMIDITY      0x02  /* bit1: humidity (1 byte, 0-100%) */
#define PRESENCE_SOIL_MOIST    0x04  /* bit2: soil moisture (1 byte, 0-100%) */
#define PRESENCE_BATTERY       0x08  /* bit3: battery (1 byte, 0-100% / 0xFF) */
#define PRESENCE_FLAGS         0x10  /* bit4: flags (1 byte) */

/* Compact packet sizing */
#define DATA_COMPACT_HEADER    3     /* node_id(1) + type(1) + presence(1) */
#define DATA_COMPACT_MAX_SIZE  9     /* header(3) + 5 payload + 1 CRC */

/* Downlink Command Bytes */
#define LORA_CMD_SET_INTERVAL       0x01
#define LORA_CMD_ON                 0x02
#define LORA_CMD_OFF                0x03
#define LORA_CMD_SET_THRESHOLD      0x04
#define LORA_CMD_SET_SCHEDULE       0x05
#define LORA_CMD_REPORT             0x06
#define LORA_CMD_TOGGLE             0x07
#define LORA_CMD_SET_MODE           0x08
#define LORA_CMD_ACK                0x09  /* Empty ACK — no command queued, node should not wait for one */
#define LORA_CMD_SET_DELTA          0x0A  /* Set individual delta threshold (type + value) */

/* Delta threshold types (for CMD_SET_DELTA param1) */
#define DELTA_TYPE_TEMPERATURE   0  /* param2 = °C × 10 (e.g. 20 = 2.0°C) */
#define DELTA_TYPE_HUMIDITY      1  /* param2 = % (e.g. 5) */
#define DELTA_TYPE_SOIL          2  /* param2 = % (e.g. 10) */
#define DELTA_TYPE_BATTERY       3  /* param2 = % (e.g. 10) */

/* Downlink Packet Structure (6 bytes) */
typedef struct __attribute__((packed)) {
    uint8_t node_id;     /* Byte 0: Node ID */
    uint8_t header;      /* Byte 1: Header (0x05) */
    uint8_t command;     /* Byte 2: Command byte */
    uint8_t param1;      /* Byte 3: Parameter 1 */
    uint8_t param2;      /* Byte 4: Parameter 2 */
    uint8_t crc;         /* Byte 5: CRC8 checksum */
} lora_downlink_packet_t;

/**
 * @brief Initialize LoRa UART module
 * 
 * Configures UART2 at 9600 baud, sets up GPIO pins for mode control,
 * and puts module in normal mode.
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t lora_uart_init(void);

/**
 * @brief Set LoRa module operating mode
 * 
 * @param mode Mode to set (LORA_MODE_NORMAL, LORA_MODE_SLEEP, etc.)
 */
void lora_set_mode(lora_mode_t mode);

/**
 * @brief Read a complete uplink packet (non-blocking)
 *
 * Reads either a legacy 8-byte packet (types 0x01-0x04) or a variable-length
 * compact packet (type 0x06, 4-9 bytes). For compact packets, the `presence`
 * field indicates which payload fields were transmitted.
 *
 * @param packet Pointer to packet structure to fill
 * @return true if a full valid packet was received and CRC verified
 * @return false if no packet available or CRC mismatch
 */
bool lora_read_packet(lora_uplink_packet_t *packet);

/**
 * @brief Send a downlink command to a node
 * 
 * Constructs a 6-byte packet with CRC and transmits via UART.
 * Waits for AUX to indicate module is ready before sending.
 * 
 * @param node_id Target node ID
 * @param command Command byte
 * @param param1 First parameter
 * @param param2 Second parameter
 * @return true if packet was sent successfully
 * @return false if send failed
 */
bool lora_send_command(uint8_t node_id, uint8_t command, uint8_t param1, uint8_t param2);

/**
 * @brief Send an empty ACK (no command) to a node
 *
 * Builds a 6-byte downlink packet with command LORA_CMD_ACK (0x09) and
 * no parameters. Used when the gateway receives an uplink but has no
 * queued command for the node, so the node does not sit waiting for an
 * ACK (which would otherwise inflate its gatewayLostCount and eventually
 * trigger a "gateway lost" alarm).
 *
 * @param node_id Target node ID
 * @return true if packet was sent successfully
 * @return false if send failed
 */
bool lora_send_ack(uint8_t node_id);

/**
 * @brief Wait for AUX pin to indicate module ready
 * 
 * @param timeout_ms Timeout in milliseconds
 * @return true if AUX went high (ready)
 * @return false if timeout
 */
bool lora_wait_aux_ready(uint32_t timeout_ms);

/**
 * @brief Flush UART receive buffer
 */
void lora_flush_rx(void);

/**
 * @brief Deinitialize LoRa module and UART
 */
void lora_uart_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* LORA_UART_H */
