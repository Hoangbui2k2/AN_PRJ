#ifndef POWER_H
#define POWER_H

#include <stdint.h>
#include "esp_sleep.h"

/* Pin definitions (from AGENT.md) */
#define SENSOR_POWER_GPIO    26   /* MOSFET gate to power sensors */
#define BUTTON_GPIO          34   /* Wake button (external wake) */

/* Deep sleep configuration. MIN/MAX must stay in sync with
 * SLEEP_INTERVAL_MIN_S / SLEEP_INTERVAL_MAX_S in config.h. */
#define DEFAULT_SLEEP_SEC    300  /* 5 minutes - initial default only */
#define MIN_SLEEP_SEC        5
#define MAX_SLEEP_SEC        3600

/**
 * @brief Initialize power management (sensor power GPIO, button interrupt)
 */
void power_init(void);

/**
 * @brief Enable sensor power (turn on MOSFET: active-low, GPIO26 = 0)
 */
void power_sensor_on(void);

/**
 * @brief Disable sensor power (turn off MOSFET: active-low, GPIO35 = 1)
 */
void power_sensor_off(void);

/**
 * @brief Check if button is pressed (with debounce)
 * @return true if pressed
 */
bool power_button_pressed(void);

/**
 * @brief Enter deep sleep for configured duration
 * Never returns - restarts on wake
 */
void power_deep_sleep(void) __attribute__((noreturn));

/**
 * @brief Enter deep sleep for a custom duration WITHOUT changing cfg->interval.
 *        Used to park awake shortly before the t=0 slot boundary.
 * Never returns - restarts on wake
 */
void power_deep_sleep_for(uint32_t sleep_sec) __attribute__((noreturn));

/**
 * @brief Get wake reason
 * @return ESP_SLEEP_WAKEUP_TIMER, ESP_SLEEP_WAKEUP_GPIO, etc.
 */
esp_sleep_wakeup_cause_t power_get_wake_cause(void);

/**
 * @brief Configure deep sleep timer
 * @param seconds Sleep duration in seconds
 */
void power_set_sleep_timer(uint32_t seconds);

#endif /* POWER_H */