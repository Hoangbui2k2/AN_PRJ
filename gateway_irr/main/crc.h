#ifndef CRC_H
#define CRC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Calculate CRC8 checksum (XOR of all bytes)
 * 
 * The CRC8 is computed as XOR of all bytes in the buffer.
 * 
 * @param data Pointer to data buffer
 * @param len Length of data in bytes
 * @return uint8_t CRC8 checksum
 */
uint8_t crc8_calculate(const uint8_t *data, size_t len);

/**
 * @brief Verify CRC8 of a received packet
 * 
 * Compares computed CRC of first (len-1) bytes with the last byte.
 * 
 * @param data Pointer to packet including CRC byte
 * @param len Total length including CRC byte
 * @return true if CRC matches
 * @return false if CRC does not match
 */
bool crc8_verify(const uint8_t *data, size_t len);

/**
 * @brief Append CRC8 to a buffer
 * 
 * Computes CRC of the first (len-1) bytes and writes it to the last byte.
 * 
 * @param data Pointer to buffer (must have space for CRC at end)
 * @param len Total length including CRC byte position
 */
void crc8_append(uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* CRC_H */
