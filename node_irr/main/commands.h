#ifndef COMMANDS_H
#define COMMANDS_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "sensors.h"

/**
 * @brief Build a data packet for sending sensor readings
 * @param node_id Node identifier
 * @param sensor_data Sensor readings
 * @param packet Output 8-byte packet buffer
 * @return 0 on success
 */
int commands_build_data_packet(uint8_t node_id, const sensor_data_t *sensor_data,
                               lora_data_packet_t *packet);

/**
 * @brief Build a compact data packet with only changed fields
 *
 * Serialises only the fields indicated by presence bitmask into a
 * variable-length buffer (4–8 bytes). Fields are written LSB-first:
 * temp, hum, soil, battery, flags — one byte each if present.
 *
 * @param node_id   Node identifier
 * @param data      Current sensor readings
 * @param presence  Bitmask of fields to include (PRESENCE_TEMP | PRESENCE_HUM | ...)
 * @param buf       Output buffer (must be at least 8 bytes)
 * @return Total packet length (bytes), or -1 on error
 */
int commands_build_compact_packet(uint8_t node_id, const sensor_data_t *data,
                                  uint8_t presence, uint8_t *buf);

/**
 * @brief Send a compact data packet with ACK verification and retries
 * @param node_id  Node identifier
 * @param data     Current sensor readings
 * @param presence Bitmask of fields to include
 * @return 0 if ACK received, -1 if all retries failed
 */
int commands_send_compact_with_ack(uint8_t node_id, const sensor_data_t *data,
                                    uint8_t presence);

/**
 * @brief Build an ACK packet
 * @param node_id Node identifier
 * @param packet Output 8-byte packet buffer
 * @return 0 on success
 */
int commands_build_ack_packet(uint8_t node_id, lora_data_packet_t *packet);

/**
 * @brief Build an ALARM packet
 * @param node_id Node identifier
 * @param alarm_code Alarm code
 * @param packet Output 8-byte packet buffer
 * @return 0 on success
 */
int commands_build_alarm_packet(uint8_t node_id, uint8_t alarm_code, lora_data_packet_t *packet);

/**
 * @brief Process a received command packet (executes, then sends ACK 0x03).
 *        Idempotent: a duplicate gateway retry is ACKed but not re-executed.
 * @param packet Received command packet (6 bytes, CRC verified)
 * @param data   Current sensor readings (used by REPORT; NULL → re-read)
 * @return 0 on success, -1 on error
 */
int commands_process(const lora_cmd_packet_t *packet, const sensor_data_t *data);

/**
 * @brief Send an ACK packet (type 0x03) immediately
 * @param node_id Node identifier
 * @return 0 on success, -1 on error
 */
int commands_send_ack(uint8_t node_id);

/**
 * @brief Send a data packet (0x01), then wait the downlink window and ACK any
 *        pending command from the gateway (spec B).
 * @param node_id Node identifier
 * @param sensor_data Sensor readings
 * @return 0 if transmitted, -1 if transport failed
 */
int commands_send_data_with_ack(uint8_t node_id, const sensor_data_t *sensor_data);

/**
 * @brief Build a heartbeat packet (type 0x02) with current flags + readings
 * @param node_id Node identifier
 * @param battery Battery level (0-100% or 0xFF for external power)
 * @param data    Current sensor readings (may be NULL)
 * @param packet Output 8-byte packet buffer
 * @return 0 on success
 */
int commands_build_heartbeat_packet(uint8_t node_id, uint8_t battery,
                                    const sensor_data_t *data, lora_data_packet_t *packet);

/**
 * @brief Send a heartbeat packet (0x02), wait the downlink window and ACK.
 *        Any valid downlink resets gatewayLostCount; no downlink increments it
 *        (spec C).  gatewayLostCount >= 3 sets GW_LOST flag + alarm 0x05.
 * @param node_id Node identifier
 * @param battery Battery level (0-100% or 0xFF for external power)
 * @param data    Current sensor readings (may be NULL)
 * @return 0 if transmitted, -1 if transport failed
 */
int commands_send_heartbeat(uint8_t node_id, uint8_t battery, const sensor_data_t *data);

/**
 * @brief Send an alarm packet (0x04), then wait the downlink window and ACK
 *        any pending command (spec D).
 * @param node_id Node identifier
 * @param alarm_code Alarm code
 * @param data    Current sensor readings (may be NULL)
 * @return 0 on success, -1 on error
 */
int commands_send_alarm(uint8_t node_id, uint8_t alarm_code, const sensor_data_t *data);

/**
 * @brief Check for pending commands from gateway (no uplink just sent)
 * @param data Current sensor readings (may be NULL)
 * @return 0 if command received and processed, 1 if no command, -1 on error
 */
int commands_check_pending(const sensor_data_t *data);

#endif /* COMMANDS_H */
