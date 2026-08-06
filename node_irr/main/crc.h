#ifndef CRC_H
#define CRC_H

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Calculate CRC8 using XOR method
 *
 * @param data Pointer to data buffer
 * @param len Length of data buffer (excludes the CRC byte itself)
 * @return uint8_t Calculated CRC8 value
 */
uint8_t crc8_xor(const uint8_t *data, size_t len);

/**
 * @brief Verify CRC8 of a complete 8-byte packet
 *
 * @param packet Pointer to 8-byte packet (CRC is the last byte)
 * @param len Total packet length (including CRC byte)
 * @return 0 on CRC match, -1 on mismatch
 */
int crc8_verify(const uint8_t *packet, size_t len);

#endif /* CRC_H */
