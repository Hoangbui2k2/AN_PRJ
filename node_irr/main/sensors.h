#ifndef SENSORS_H
#define SENSORS_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_adc/adc_oneshot.h"

/**
 * @brief Sensor data structure passed between modules
 */
typedef struct {
    float    temperature;        /* DHT22 temperature in Celsius */
    float    humidity;           /* DHT22 humidity in %RH */
    uint16_t soil_moisture_raw;  /* ADC raw value (0-4095) */
    uint8_t  soil_moisture_pct;  /* Mapped soil moisture percentage (0-100%) */
    uint8_t  battery;            /* Battery level (0-100%) or 0xFF for external power */
    int      temperature_int;    /* Temperature * 10 for LoRa packet */
    int      humidity_int;       /* Humidity * 10 for LoRa packet */
    uint8_t  sensor_error;       /* Bitmask: bit0=DHT, bit1=soil */
} sensor_data_t;

/* GPIO and ADC definitions (from AGENT.md) */
#define DHT22_GPIO             33
#define SOIL_MOISTURE_ADC_CH   ADC_CHANNEL_4  /* GPIO32 */
#define SOIL_MOISTURE_ADC_UNIT ADC_UNIT_1

/* Battery measurement — external voltage divider (100k+100k) on GPIO36 */
#define BATTERY_ADC_GPIO       36
#define BATTERY_ADC_CH         ADC_CHANNEL_0  /* ADC1_CH0 = GPIO36 = SENSOR_VP */
#define BATTERY_DIVIDER_RATIO  2.0f           /* Vbat = Vdiv * 2 */

/* Battery voltage thresholds for 18650 Li-ion */
#define BATTERY_VOLT_FULL      4.20f          /* 100% */
#define BATTERY_VOLT_EMPTY     3.30f          /* 0% — safe cutoff */

/**
 * @brief Initialize sensor subsystem (configure ADC, init DHT)
 */
void sensors_init(void);

/**
 * @brief Read all sensors
 * @param data Pointer to sensor_data_t to fill
 */
void sensors_read(sensor_data_t *data);

/**
 * @brief Get last sensor data
 * @param data Pointer to sensor_data_t to fill
 * @return true if valid data exists
 */
bool sensors_get_last(sensor_data_t *data);

/**
 * @brief Check if DHT sensor had error on last read
 * @return true if error
 */
bool sensors_dht_error(void);

/**
 * @brief Check if soil sensor had error on last read
 * @return true if error
 */
bool sensors_soil_error(void);

#endif /* SENSORS_H */
