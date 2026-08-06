#ifndef ALARMS_H
#define ALARMS_H

#include <stdint.h>
#include "config.h"

/* Pin definition */
#define ALARM_LED_GPIO    13

/* Alarm blink timing */
#define ALARM_BLINK_MS        100   /* LED on duration per blink */
#define ALARM_BLINK_GAP_MS    200   /* Gap between blinks in a pattern */
#define ALARM_PATTERN_GAP_MS  1000  /* Gap between pattern repeats */
#define ALARM_REPEAT_COUNT    3     /* Number of pattern repeats */

/* Number of blinks per alarm code */
#define BLINKS_SENSOR_ERROR   1
#define BLINKS_SOIL_OUT_RANGE 2
#define BLINKS_RELAY_ERROR    3
#define BLINKS_LOW_BATTERY    4
#define BLINKS_GATEWAY_LOST   5

/**
 * @brief Initialize alarm LED GPIO
 */
void alarms_init(void);

/**
 * @brief Execute blink pattern for given alarm (blocking)
 * @param alarm Alarm code (ALARM_SENSOR_ERROR etc.)
 */
void alarms_signal(alarm_type_t alarm);

/**
 * @brief Light alarm LED briefly
 */
void alarm_led_on(void);

/**
 * @brief Turn off alarm LED
 */
void alarm_led_off(void);

/**
 * @brief Map alarm code to blink count
 * @return Number of blinks for this alarm type
 */
int alarms_get_blink_count(alarm_type_t alarm);

#endif /* ALARMS_H */
