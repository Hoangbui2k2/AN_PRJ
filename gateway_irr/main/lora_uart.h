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
/* Max time to wait for AUX to indicate the module is idle before a new send */
#define LORA_TX_TIMEOUT_MS 100
/* Max time to wait for AUX to be pulled LOW (module started the RF burst) */
#define LORA_TX_BUSY_TIMEOUT_MS 100
/* Max time to wait for AUX to return HIGH after the RF burst is finished.
 * 9 bytes at the default air rate take well under 100 ms; give it margin. */
#define LORA_TX_IDLE_TIMEOUT_MS 300

/* Packet Definitions */
#define LORA_UPLINK_SIZE   8   /* Uplink packet size (Node -> Gateway, legacy/ACK) */
#define LORA_DOWNLINK_SIZE 7   /* Downlink command size (v2: +slot byte) */
#define LORA_CMD_HEADER    0x05 /* Command header byte for downlink */

/* ── v2 packet types (slot + baseline) ── */
#define PKT_TYPE_BASELINE      0x07  /* GW -> Node: baseline chunk (variable) */
#define PKT_TYPE_BASELINE_DONE 0x08  /* Node -> GW: one baseline series stored */
#define PKT_TYPE_REQ           0x09  /* Node -> GW: request (time and/or baseline) */

/* Request flags (PKT_TYPE_REQ, payload byte) */
#define REQ_FLAG_BASELINE  0x01
#define REQ_FLAG_TIME      0x02

/* Baseline series ids */
#define BASELINE_SERIES_TEMP  0
#define BASELINE_SERIES_HUM   1
#define BASELINE_SERIES_SOIL  2
#define BASELINE_SERIES_COUNT 3

/* Baseline chunk sizing: header(7) + n*2 payload + crc.
 * BASELINE_MAX_POINTS = one point per 15-minute slot (96 slots/day). Points are
 * sparse: fewer than 96 entries is normal and interpolation fills the gaps. */
#define BASELINE_MAX_POINTS        96
#define BASELINE_POINTS_PER_CHUNK  10
/* Max chunk frame = dest|type|series|version|seq|total|n_points (7 B)
 * + n_points * [t,y] (2 B each) + crc (1 B). Derived so raising
 * BASELINE_POINTS_PER_CHUNK can never leave the buffers too small. */
#define BASELINE_MAX_FRAME         (7 + 2 * BASELINE_POINTS_PER_CHUNK + 1)
#define BASELINE_MAX_CHUNKS        ((BASELINE_MAX_POINTS + BASELINE_POINTS_PER_CHUNK - 1) / BASELINE_POINTS_PER_CHUNK)

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

/* Downlink Packet Structure (7 bytes, v2)
 *   0 node_id | 1 header(0x05) | 2 command | 3 param1 | 4 param2 | 5 slot | 6 crc
 * `slot` carries the gateway's current 15-minute slot (0..95) in EVERY
 * downlink so the node is time-synced on any exchange.
 */
typedef struct __attribute__((packed)) {
    uint8_t node_id;     /* Byte 0: Node ID (0xFF = broadcast) */
    uint8_t header;      /* Byte 1: Header (0x05) */
    uint8_t command;     /* Byte 2: Command byte */
    uint8_t param1;      /* Byte 3: Parameter 1 */
    uint8_t param2;      /* Byte 4: Parameter 2 */
    uint8_t slot;        /* Byte 5: current slot 0..95 */
    uint8_t crc;         /* Byte 6: CRC8 checksum of bytes 0-5 */
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
 * Constructs a 7-byte packet (v2, includes the current slot) with CRC and
 * transmits via UART. Waits for AUX to indicate module is ready before sending.
 *
 * @param node_id Target node ID (0xFF = broadcast)
 * @param command Command byte
 * @param param1 First parameter
 * @param param2 Second parameter
 * @param slot   Current 15-minute slot (0..SLOT_MAX)
 * @return true if packet was sent successfully
 */
bool lora_send_command(uint8_t node_id, uint8_t command, uint8_t param1,
                       uint8_t param2, uint8_t slot);

/**
 * @brief Send an empty ACK (no command) to a node
 *
 * Builds a 7-byte downlink packet with command LORA_CMD_ACK (0x09) and no
 * parameters, carrying the current slot. Used when the gateway receives an
 * uplink but has no queued command for the node, so the node does not sit
 * waiting for an ACK (which would otherwise inflate its gatewayLostCount and
 * eventually trigger a "gateway lost" alarm).
 *
 * @param node_id Target node ID
 * @param slot    Current slot 0..SLOT_MAX
 * @return true if packet was sent successfully
 */
bool lora_send_ack(uint8_t node_id, uint8_t slot);

/**
 * @brief Send one baseline chunk (type 0x07) to a node
 *
 * Frame: dest|0x07|series|version|seq|total|n_points|[t,y]xn|crc
 *
 * @param node_id  Target node ID
 * @param series   BASELINE_SERIES_*
 * @param version  Baseline version
 * @param seq      Chunk index (0..total-1)
 * @param total    Total chunks for this series
 * @param points   Packed [t,y] pairs (2 bytes each)
 * @param n_points Number of points in this chunk (<= BASELINE_POINTS_PER_CHUNK)
 * @return true if packet was sent successfully
 */
bool lora_send_baseline_chunk(uint8_t node_id, uint8_t series, uint8_t version,
                              uint8_t seq, uint8_t total,
                              const uint8_t *points, uint8_t n_points);

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
