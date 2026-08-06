#include "crc.h"

/**
 * @brief Calculate CRC8 as simple XOR of all bytes
 * 
 * Per the specification: CRC8 = XOR of bytes 0 through (len-1)
 */
uint8_t crc8_calculate(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
    }
    return crc;
}

bool crc8_verify(const uint8_t *data, size_t len)
{
    if (len < 1) {
        return false;
    }
    uint8_t expected = crc8_calculate(data, len - 1);
    return (expected == data[len - 1]);
}

void crc8_append(uint8_t *data, size_t len)
{
    if (len < 1) {
        return;
    }
    data[len - 1] = crc8_calculate(data, len - 1);
}
