#include "crc.h"

/**
 * @brief CRC8 XOR calculation
 * Simple XOR of all bytes - no polynomial, just parity
 */
uint8_t crc8_xor(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
    }
    return crc;
}

/**
 * @brief Verify CRC8 on an 8-byte packet
 * The CRC is the 8th byte (index 7), calculated from bytes 0-6
 */
int crc8_verify(const uint8_t *packet, size_t len)
{
    if (!packet || len != 8) {
        return -1;
    }

    uint8_t expected = crc8_xor(packet, 7);
    uint8_t received = packet[7];

    return (expected == received) ? 0 : -1;
}