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
 * @brief Start a timed pump run (the pump must already be ON).
 *
 * Used by the schedule and by "relay ON for N seconds". The deadline is kept on
 * the RTC clock (monotonic across deep sleep) and the deep-sleep interval is
 * shortened so the node wakes up to stop the pump — but an EARLIER wake (button,
 * park near t=0, gateway polling) no longer cuts the run short: the pump keeps
 * running and the node re-sleeps for the remaining time.
 *
 * @param duration_s Run time in seconds (clamped to MIN/MAX_SLEEP_SEC)
 */
void irrigation_start_timed_run(uint16_t duration_s);

/**
 * @brief End a timed run (Relay OFF / manual toggle): clears the run flag and
 *        the deadline and restores the normal deep-sleep interval.
 */
void irrigation_cancel_timed_run(void);

/**
 * @brief Seconds left of the active timed run.
 * @return 0 when no timed run is active or its deadline has already elapsed
 */
uint32_t irrigation_timed_run_remaining_s(void);

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
