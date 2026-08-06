#ifndef IRRIGATION_H
#define IRRIGATION_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "sensors.h"

/**
 * @brief Initialize irrigation subsystem
 */
void irrigation_init(void);

/**
 * @brief Evaluate irrigation needs based on current mode and sensor data
 * @param data Sensor readings
 * @return true if irrigation should be active, false otherwise
 */
bool irrigation_should_irrigate(const sensor_data_t *data);

/**
 * @brief Main irrigation cycle - call once per wake cycle
 * @param data Sensor readings
 * @return 0 on success, -1 on failure
 */
int irrigation_cycle(const sensor_data_t *data);

/**
 * @brief Check if irrigation is currently active (pump ON)
 */
bool irrigation_is_active(void);

/**
 * @brief Handle the case when gateway is lost (3 failed ACKs)
 */
void irrigation_gateway_lost(void);

#endif /* IRRIGATION_H */
