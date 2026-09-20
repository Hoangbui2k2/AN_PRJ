#include "irrigation.h"
#include "pump.h"
#include "power.h"
#include "config.h"
#include "esp_log.h"
#include "esp_private/esp_clk.h"   /* esp_clk_rtc_time(): RTC clock, alive across deep sleep */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "IRRIGATION";

static bool s_irrigation_active = false;

/* ──────────── Timed runs (schedule + "relay ON for N s") ────────────
 *
 * The run must survive deep sleep AND any extra wake that happens before its
 * deadline (button press, park near t=0, gateway polling). Keeping the deadline
 * on the RTC clock lets irrigation_cycle() distinguish "still running" from
 * "duration elapsed" instead of stopping the pump on the first wake. */
RTC_DATA_ATTR static uint64_t s_timed_run_end_us = 0;   /* 0 = no timed run */

void irrigation_start_timed_run(uint16_t duration_s)
{
    app_config_t *cfg = config_get();

    if (duration_s < MIN_SLEEP_SEC) duration_s = MIN_SLEEP_SEC;
    if (duration_s > MAX_SLEEP_SEC) duration_s = MAX_SLEEP_SEC;

    cfg->pumpBySchedule   = true;
    cfg->scheduleDuration = duration_s;
    cfg->interval         = duration_s;   /* wake up again to stop the pump */
    config_save();

    uint64_t now_us = esp_clk_rtc_time();
    s_timed_run_end_us = (now_us != 0)
                             ? now_us + (uint64_t)duration_s * 1000000ULL
                             : 0;   /* no RTC time → stop on the next wake */

    ESP_LOGI(TAG, "Timed run started: %u s (deadline %s)", duration_s,
             s_timed_run_end_us ? "on the RTC clock" : "next wake (RTC time unset)");
}

void irrigation_cancel_timed_run(void)
{
    app_config_t *cfg = config_get();

    if (!cfg->pumpBySchedule && s_timed_run_end_us == 0) return;

    cfg->pumpBySchedule = false;
    s_timed_run_end_us  = 0;

    if (cfg->interval != cfg->normalInterval) {
        cfg->interval = cfg->normalInterval;
        ESP_LOGI(TAG, "Sleep interval restored to %u s", cfg->interval);
    }
    config_save();
    ESP_LOGI(TAG, "Timed run cancelled");
}

uint32_t irrigation_timed_run_remaining_s(void)
{
    app_config_t *cfg = config_get();

    if (!cfg->pumpBySchedule) return 0;
    if (s_timed_run_end_us == 0) return 0;   /* unknown deadline → ends next wake */

    uint64_t now_us = esp_clk_rtc_time();
    if (now_us == 0 || now_us >= s_timed_run_end_us) return 0;

    uint32_t remain_s = (uint32_t)((s_timed_run_end_us - now_us) / 1000000ULL);
    return (remain_s == 0) ? 1 : remain_s;
}

void irrigation_init(void)
{
    ESP_LOGI(TAG, "Initializing irrigation subsystem");
    s_irrigation_active = false;
}

/* ──────────── Schedule on the 15-minute time axis (t) ────────────
 *
 * The node has NO absolute clock: it only receives `slot` (t = 0..95, one slot
 * per 15 minutes) from the gateway on every downlink. The schedule is therefore
 * evaluated on that axis:
 *
 *   target_t = (hour * 60 + minute) / 15        (minutes snap DOWN to a slot)
 *
 * "Once per day" comes from the slot-day counter, which is incremented at every
 * t=0 (midnight) wrap — no epoch and no timezone needed. */

/* A run is still executed up to this many slots (×15 min) after its target
 * slot, so a wake that lands just past the target still waters. */
#define SCHEDULE_CATCHUP_SLOTS  1

/**
 * @brief Slot (0..95) of the configured schedule time.
 */
static uint8_t schedule_target_slot(void)
{
    const app_config_t *cfg = config_get();
    uint32_t mins = (uint32_t)cfg->scheduleHour * 60u + cfg->scheduleMinute;
    uint32_t slot = mins / SLOT_MINUTES;
    return (slot > SLOT_MAX) ? (uint8_t)SLOT_MAX : (uint8_t)slot;
}

/**
 * @brief Should the scheduled watering start on this wake?
 *
 * Fires when the current slot is the target slot, or within
 * SCHEDULE_CATCHUP_SLOTS after it, once per day (slot-day counter). Requires a
 * synced slot and reports nothing while `t` is unknown.
 */
static bool is_schedule_time(void)
{
    app_config_t *cfg = config_get();

    if (!config_slot_valid()) {
        ESP_LOGW(TAG, "Slot t not synced yet - schedule check skipped");
        return false;
    }

    uint16_t day = config_get_slot_day();
    if (day == cfg->lastWateringDay) {
        ESP_LOGD(TAG, "Schedule already executed on slot-day %u", day);
        return false;
    }

    uint8_t target   = schedule_target_slot();
    uint8_t now_slot = config_get_slot();

    /* Too early, or so late that the catch-up window has passed (the latter also
     * avoids "watering immediately" when a past time is set from the server). */
    if (now_slot < target ||
        (uint8_t)(now_slot - target) > SCHEDULE_CATCHUP_SLOTS) {
        return false;
    }

    ESP_LOGI(TAG, "Schedule window hit: t=%u (target t=%u = %02u:%02u), day %u",
             now_slot, target, cfg->scheduleHour, cfg->scheduleMinute, day);

    /* Mark + persist "done for this day" so a reset cannot repeat it. */
    config_set_last_watering_day(day);
    return true;
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

    /* ── Step 1: A timed run (schedule / relay-ON duration) may be running ──
     * Only stop the pump once its deadline has really passed. An early wake
     * (button, park, gateway polling) keeps the pump ON and re-arms the sleep
     * for whatever is left of the run. */
    if (cfg->pumpBySchedule) {
        uint32_t remain_s = irrigation_timed_run_remaining_s();

        if (remain_s > 0) {
            cfg->interval = (uint16_t)remain_s;
            config_save();
            ESP_LOGI(TAG, "Timed run still active (%lu s left) - pump stays ON",
                     (unsigned long)remain_s);
            return 0;
        }

        ESP_LOGI(TAG, "Scheduled duration elapsed. Turning pump OFF.");
        if (pump_off()) {
            irrigation_cancel_timed_run();   /* clears flag + restores interval */
            ESP_LOGI(TAG, "Timed run complete. Deep sleep interval restored to %u s",
                     cfg->interval);
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
                irrigation_start_timed_run(cfg->scheduleDuration);
                ESP_LOGI(TAG, "Pump turned ON by schedule - sleeping %u s until the run ends",
                         cfg->interval);
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
    app_config_t *cfg = config_get();

    /* Only the AUTOMATIC mode (THRESHOLD) falls back to the schedule when the
     * gateway disappears. MANUAL stays manual (the operator is in charge) and a
     * node that is already in SCHEDULE simply keeps its schedule. */
    if (cfg->mode != MODE_THRESHOLD) {
        ESP_LOGW(TAG, "Gateway lost in mode %u - no automatic fallback "
                 "(only THRESHOLD falls back to SCHEDULE)", (unsigned)cfg->mode);
        return;
    }

    ESP_LOGW(TAG, "Gateway lost in THRESHOLD mode - falling back to SCHEDULE mode");
    config_set_mode(MODE_SCHEDULE);
    /* NOTE: gatewayLostCount/gatewayLost are intentionally NOT reset here.
     * They stay raised until a valid downlink resets them, so FLAG_GATEWAY_LOST
     * (bit2) remains set in all uplinks while the gateway is unreachable
     * (spec C). */
}
