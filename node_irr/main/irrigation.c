#include "irrigation.h"
#include "pump.h"
#include "power.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>

static const char *TAG = "IRRIGATION";

static bool s_irrigation_active = false;

void irrigation_init(void)
{
    ESP_LOGI(TAG, "Initializing irrigation subsystem");
    s_irrigation_active = false;
}

/**
 * @brief Check if current time matches schedule window
 * @return true if scheduled watering should trigger
 */
static bool is_schedule_time(void)
{
    app_config_t *cfg = config_get();
    time_t now = time(NULL);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    /* If the system clock is unset (defaulting to epoch 0 or close to it),
     * we cannot evaluate schedule. It needs a time sync first. */
    if (now < 86400) {
        ESP_LOGW(TAG, "System time not synchronized (epoch < 1 day) - skipping schedule check");
        return false;
    }

    /* Track daily schedule trigger to prevent double activation */
    if (timeinfo.tm_yday == cfg->lastWateringDay) {
        ESP_LOGD(TAG, "Schedule already executed today (%d)", timeinfo.tm_yday);
        return false;
    }

    uint32_t current_mins = (uint32_t)timeinfo.tm_hour * 60 + timeinfo.tm_min;
    uint32_t scheduled_mins = (uint32_t)cfg->scheduleHour * 60 + cfg->scheduleMinute;

    if (current_mins >= scheduled_mins) {
        /* Record that we triggered watering today */
        cfg->lastWateringDay = timeinfo.tm_yday;
        config_save();
        return true;
    }

    return false;
}

bool irrigation_should_irrigate(const sensor_data_t *data)
{
    app_config_t *cfg = config_get();

    switch (cfg->mode) {
        case MODE_MANUAL:
            /* In manual mode, we do not automatically toggle pump */
            return cfg->pumpState;

        case MODE_SCHEDULE:
            return is_schedule_time();

        case MODE_THRESHOLD:
            if (data->sensor_error & 0x02) {
                ESP_LOGE(TAG, "Soil moisture sensor error in threshold mode - fallback to Schedule Mode");
                config_set_alarm(ALARM_SENSOR_ERROR);
                config_set_mode(MODE_SCHEDULE);
                return false;
            }

            if (s_irrigation_active) {
                /* Currently irrigating: stop when moisture > thresholdHigh */
                return (data->soil_moisture_pct < cfg->thresholdHigh);
            } else {
                /* Not irrigating: start when moisture < thresholdLow */
                return (data->soil_moisture_pct < cfg->thresholdLow);
            }

        default:
            ESP_LOGW(TAG, "Unknown mode: %d", cfg->mode);
            return false;
    }
}

int irrigation_cycle(const sensor_data_t *data)
{
    app_config_t *cfg = config_get();

    /* ── Step 1: Check if pump was turned on by schedule and needs to turn off ── */
    if (cfg->pumpBySchedule) {
        ESP_LOGI(TAG, "Scheduled duration elapsed. Turning pump OFF.");
        if (pump_off()) {
            cfg->pumpBySchedule = false;
            /* Restore normal deep sleep interval */
            cfg->interval = cfg->normalInterval;
            config_save();
            ESP_LOGI(TAG, "Schedule watering complete. Deep sleep interval restored to %u s", cfg->interval);
        } else {
            ESP_LOGE(TAG, "Failed to turn pump OFF");
            config_set_alarm(ALARM_RELAY_ERROR);
            return -1;
        }
        return 0;
    }

    /* ── Step 2: Evaluate normal mode operation ── */
    bool should_irrigate = irrigation_should_irrigate(data);

    ESP_LOGI(TAG, "Irrigation evaluation: mode=%d, pump_state=%d, should_irrigate=%d",
             cfg->mode, cfg->pumpState, should_irrigate);

    if (cfg->mode == MODE_SCHEDULE) {
        if (should_irrigate && !cfg->pumpState) {
            ESP_LOGI(TAG, "Scheduled time reached. Turning pump ON.");
            if (pump_on()) {
                cfg->pumpBySchedule = true;
                /* Temporarily shorten deep sleep interval to pump runtime so we wake up to turn it off */
                cfg->interval = cfg->scheduleDuration;
                config_save();
                ESP_LOGI(TAG, "Pump turned ON by schedule. Sleeping for %u s duration.", cfg->interval);
            } else {
                ESP_LOGE(TAG, "Failed to start schedule watering");
                config_set_alarm(ALARM_RELAY_ERROR);
                return -1;
            }
        }
    } else if (cfg->mode == MODE_THRESHOLD) {
        if (should_irrigate && !cfg->pumpState) {
            ESP_LOGI(TAG, "Moisture below threshold low. Turning pump ON.");
            if (pump_on()) {
                s_irrigation_active = true;
            } else {
                ESP_LOGE(TAG, "Failed to turn pump ON");
                config_set_alarm(ALARM_RELAY_ERROR);
                return -1;
            }
        } else if (!should_irrigate && cfg->pumpState) {
            ESP_LOGI(TAG, "Moisture above threshold high. Turning pump OFF.");
            if (pump_off()) {
                s_irrigation_active = false;
            } else {
                ESP_LOGE(TAG, "Failed to turn pump OFF");
                config_set_alarm(ALARM_RELAY_ERROR);
                return -1;
            }
        }
    }

    return 0;
}

bool irrigation_is_active(void)
{
    return s_irrigation_active;
}

void irrigation_gateway_lost(void)
{
    ESP_LOGW(TAG, "Gateway lost - falling back to Schedule mode");
    config_set_mode(MODE_SCHEDULE);
    /* NOTE: gatewayLostCount/gatewayLost are intentionally NOT reset here.
     * They stay raised until a valid downlink resets them, so FLAG_GATEWAY_LOST
     * (bit2) remains set in all uplinks while the gateway is unreachable
     * (spec C). */
}
