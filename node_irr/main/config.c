#include "config.h"
#include "sensors.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "CONFIG";

/* Allocate configuration structure in RTC memory */
RTC_DATA_ATTR app_config_t rtc_config;

void config_init_default(void)
{
    ESP_LOGI(TAG, "Initializing default configuration");
    rtc_config.magic = CONFIG_MAGIC;
    rtc_config.interval = NODE_SLEEP_INTERVAL_S;   /* default 300 s (CMD_SET_INTERVAL can change) */
    rtc_config.thresholdLow = 30;       /* 30% soil moisture */
    rtc_config.thresholdHigh = 70;      /* 70% soil moisture */
    rtc_config.scheduleHour = 6;        /* 06:00 */
    rtc_config.scheduleMinute = 0;
    rtc_config.scheduleDuration = 60;   /* 60 seconds pump ON */
    rtc_config.mode = MODE_THRESHOLD;   /* Default mode is Threshold (Mode 2) */
    rtc_config.nodeId = NODE_ID;
    rtc_config.sensorErrorCount = 0;
    rtc_config.gatewayLostCount = 0;
    rtc_config.gatewayLost = false;
    rtc_config.pumpState = false;
    rtc_config.thresholdExceeded = false;
    rtc_config.cyclesSinceSend = 0;
    rtc_config.alarmCode = ALARM_NONE;
    rtc_config.lastAlarmCode = ALARM_NONE;
    rtc_config.persistedAlarmCode = ALARM_NONE;
    rtc_config.totalPumpCycles = 0;
    rtc_config.lastScheduleTime = 0;
    rtc_config.pumpBySchedule = false;
    rtc_config.normalInterval = NODE_SLEEP_INTERVAL_S;
    rtc_config.lastWateringDay = 0xFFFF; /* Schedule never executed yet */
    rtc_config.lastSentTemp = INT8_MIN; /* Never sent */
    rtc_config.lastSentHumidity = 0;
    rtc_config.lastSentSoil = 0;
    rtc_config.lastSentBattery = 0xFF;
    rtc_config.lastSentFlags = 0;
    rtc_config.deltaTemp = TEMP_DELTA_THRESHOLD;
    rtc_config.deltaHumidity = HUM_DELTA_THRESHOLD;
    rtc_config.deltaSoil = SOIL_DELTA_THRESHOLD;
    rtc_config.deltaBattery = BATTERY_DELTA_THRESHOLD;

    /* Baseline time axis — invalid until the first slot sync from the gateway.
     * slotValid=false makes the boot flow request a time sync after any reset
     * that clears RTC memory. */
    rtc_config.currentSlot = 0;
    rtc_config.slotValid = false;
    rtc_config.slotDay = 0;
    rtc_config.slotElapsedUs = 0;
    rtc_config.slotErrorUs = 0;
}

bool config_load(void)
{
    if (rtc_config.magic != CONFIG_MAGIC) {
        ESP_LOGW(TAG, "RTC config invalid (magic: 0x%08X), loading defaults", (unsigned int)rtc_config.magic);
        config_init_default();
        return false;
    }
    ESP_LOGI(TAG, "RTC config loaded successfully");
    return true;
}

void config_save(void)
{
    rtc_config.magic = CONFIG_MAGIC;
}

app_config_t *config_get(void)
{
    return &rtc_config;
}

void config_set_alarm(uint8_t alarm)
{
    if (rtc_config.alarmCode != alarm) {
        rtc_config.alarmCode = alarm;
        ESP_LOGI(TAG, "Alarm set to: 0x%02X", alarm);
        /* Snapshot the active alarm into persisted storage so a later
         * power-loss / cold-boot knows the alarm was active. */
        if (alarm != ALARM_NONE) {
            rtc_config.persistedAlarmCode = alarm;
            save_alarm_state_to_nvs();
        }
    }
}

void config_clear_alarm(void)
{
    if (rtc_config.alarmCode != ALARM_NONE) {
        ESP_LOGI(TAG, "Clearing active alarm (0x%02X)", rtc_config.alarmCode);
        rtc_config.alarmCode = ALARM_NONE;
        /* persistedAlarmCode intentionally left untouched here — it records
         * the alarm that was active (for post-boot comparison) and is only
         * reset once recovery has run. */
    }
}

uint8_t config_get_alarm(void)
{
    return rtc_config.alarmCode;
}

uint8_t config_get_last_alarm(void)
{
    return rtc_config.lastAlarmCode;
}

void config_set_last_alarm(uint8_t code)
{
    if (rtc_config.lastAlarmCode != code) {
        rtc_config.lastAlarmCode = code;
        save_alarm_state_to_nvs();   /* persist to flash (save-on-change) */
    }
}

uint8_t config_get_persisted_alarm(void)
{
    return rtc_config.persistedAlarmCode;
}

void config_set_persisted_alarm(uint8_t code)
{
    if (rtc_config.persistedAlarmCode != code) {
        rtc_config.persistedAlarmCode = code;
        save_alarm_state_to_nvs();
    }
}

void config_set_pump_state(bool on)
{
    if (rtc_config.pumpState != on) {
        rtc_config.pumpState = on;
        if (on) {
            rtc_config.totalPumpCycles++;
        }
        ESP_LOGI(TAG, "Pump state updated to: %s (Total cycles: %lu)",
                 on ? "ON" : "OFF", (unsigned long)rtc_config.totalPumpCycles);
    }
}

void config_toggle_pump(void)
{
    config_set_pump_state(!rtc_config.pumpState);
}

void config_set_mode(operation_mode_t mode)
{
    rtc_config.mode = mode;
    ESP_LOGI(TAG, "Mode set to: %d", mode);
}

void config_set_sleep_time(uint16_t seconds)
{
    /* Clamp to the range the gateway also accepts (5 s .. 3600 s). */
    if (seconds < SLEEP_INTERVAL_MIN_S) seconds = SLEEP_INTERVAL_MIN_S;
    if (seconds > SLEEP_INTERVAL_MAX_S) seconds = SLEEP_INTERVAL_MAX_S;

    /* Always keep the "normal" interval up to date. cfg->interval is only
     * overwritten when NO timed run is active: during a run it holds the
     * remaining run time, and irrigation_cancel_timed_run() restores it from
     * normalInterval when the run finishes. */
    rtc_config.normalInterval = seconds;
    if (!rtc_config.pumpBySchedule) {
        rtc_config.interval = seconds;
    }
    config_save();

    ESP_LOGI(TAG, "Sleep interval set to: %u s (normal=%u, active=%u, timed_run=%d)",
             seconds, rtc_config.normalInterval, rtc_config.interval,
             rtc_config.pumpBySchedule ? 1 : 0);
}

void config_set_schedule(uint8_t hour, uint8_t minute)
{
    /* The schedule runs on the fixed 15-minute time axis (t), not on an absolute
     * clock (the node never receives an epoch — only `slot`). The requested time
     * is therefore mapped to a slot; minutes snap DOWN to the enclosing slot
     * (e.g. 06:20 → t=25 = 06:15).
     * lastWateringDay is intentionally NOT re-armed here: a time that is already
     * in the past for today will take effect at the next day boundary instead of
     * watering immediately. */
    uint32_t mins = (uint32_t)hour * 60u + minute;
    uint32_t slot = mins / SLOT_MINUTES;
    if (slot > SLOT_MAX) slot = SLOT_MAX;

    if ((minute % SLOT_MINUTES) != 0) {
        ESP_LOGW(TAG, "Schedule minute %u is not a multiple of %u — snapped to t=%lu",
                 minute, (unsigned)SLOT_MINUTES, (unsigned long)slot);
    }

    rtc_config.scheduleHour = hour;
    rtc_config.scheduleMinute = minute;
    ESP_LOGI(TAG, "Schedule set to: %02u:%02u -> slot t=%lu",
             hour, minute, (unsigned long)slot);
}

void config_reset_gw_lost(void)
{
    rtc_config.gatewayLostCount = 0;
    rtc_config.gatewayLost = false;
}

void config_increment_gw_lost(void)
{
    if (rtc_config.gatewayLostCount < 250) {
        rtc_config.gatewayLostCount++;
    }
    if (rtc_config.gatewayLostCount >= GW_LOST_ALARM_THRESHOLD) {
        rtc_config.gatewayLost = true;
    }
    ESP_LOGW(TAG, "Gateway lost count: %d", rtc_config.gatewayLostCount);
}

void config_reset_sensor_errors(void)
{
    rtc_config.sensorErrorCount = 0;
}

void config_increment_sensor_errors(void)
{
    rtc_config.sensorErrorCount++;
    ESP_LOGW(TAG, "Sensor error count: %d", rtc_config.sensorErrorCount);
}

/* ──────────── Threshold Functions ──────────── */

void config_set_threshold_exceeded(bool exceeded)
{
    rtc_config.thresholdExceeded = exceeded;
}

bool node_check_threshold(uint8_t soil_moisture_pct)
{
    if (soil_moisture_pct < rtc_config.thresholdLow ||
        soil_moisture_pct > rtc_config.thresholdHigh) {
        rtc_config.thresholdExceeded = true;
        return true;  /* Exceeded threshold — should send data */
    }
    rtc_config.thresholdExceeded = false;
    return false;     /* Within threshold — may skip send */
}

bool is_heartbeat_time(void)
{
    return (rtc_config.cyclesSinceSend >= HEARTBEAT_CYCLES);
}

void increment_cycle_counter(void)
{
    rtc_config.cyclesSinceSend++;
}

void reset_cycle_counter(void)
{
    rtc_config.cyclesSinceSend = 0;
}

/* ──────────── Baseline time axis (15-minute slots) ──────────── */

/* ──────────── Slot-day counter (daily schedule basis) ──────────── */

/** One full lap of the 15-minute time axis = one day. Persist immediately: a
 *  day boundary happens only ~once per 24 h, so the flash cost is negligible. */
static void config_slot_day_bump(void)
{
    rtc_config.slotDay++;
    save_schedule_state_to_nvs();
    ESP_LOGI(TAG, "Slot day counter -> %u (t=0 wrap)", rtc_config.slotDay);
}

uint16_t config_get_slot_day(void)
{
    return rtc_config.slotDay;
}

uint16_t config_get_last_watering_day(void)
{
    return rtc_config.lastWateringDay;
}

void config_set_last_watering_day(uint16_t day)
{
    rtc_config.lastWateringDay = day;
    save_schedule_state_to_nvs();
    ESP_LOGI(TAG, "Schedule marked as executed on slot-day %u", day);
}

void config_set_current_slot(uint8_t slot)
{
    if (slot > SLOT_MAX) slot = SLOT_MAX;

    if (!rtc_config.slotValid) {
        ESP_LOGI(TAG, "Slot synced (first): %u", slot);
        rtc_config.slotErrorUs = 0;
    } else {
        /* Measure the local estimate before overwriting it.
         *   local   = currentSlot * SLOT_US + slotElapsedUs
         *   gateway = slot * SLOT_US (+ an unknown fraction of the slot)
         * phase = local − gateway. While both sides are in the SAME slot the
         * phase is simply slotElapsedUs (0..SLOT_US). A wrapped slot-index
         * difference `d != 0`, or a phase outside [0, SLOT_US), means the local
         * numbering had drifted by at least one whole slot. */
        int32_t d = (int32_t)rtc_config.currentSlot - (int32_t)slot;
        if (d >  SLOTS_PER_DAY / 2) d -= SLOTS_PER_DAY;
        if (d < -(SLOTS_PER_DAY / 2)) d += SLOTS_PER_DAY;
        rtc_config.slotErrorUs = (int32_t)((int64_t)d * (int64_t)SLOT_US +
                                           (int64_t)rtc_config.slotElapsedUs);

        ESP_LOGI(TAG, "Slot sync: %u -> %u | local phase %lu ms in slot, "
                 "slot-index delta %+d (%s)",
                 rtc_config.currentSlot, slot,
                 (unsigned long)(rtc_config.slotElapsedUs / 1000UL), (int)d,
                 (d == 0 && rtc_config.slotElapsedUs < SLOT_US) ? "OK" : "DRIFT >= 1 slot");
    }

    /* Day counter: only a LARGE backward jump (typically 95 -> 0 at midnight) is
     * a real day boundary. Small backward corrections are just the gateway
     * pulling a slightly-ahead estimate back, and must not inflate the day. */
    if (rtc_config.slotValid &&
        ((int)rtc_config.currentSlot - (int)slot) > (SLOTS_PER_DAY / 2)) {
        config_slot_day_bump();
    }

    rtc_config.currentSlot = slot;
    rtc_config.slotValid = true;
    rtc_config.slotElapsedUs = 0;
}

void config_advance_slot_us(uint64_t elapsed_us)
{
    /* Nothing to advance from until the gateway has sent a slot at least once. */
    if (!rtc_config.slotValid) return;

    /* Keep µs precision: the old second-resolution accumulator lost up to 1 s
     * per cycle, which is ~5 minutes per day on its own. */
    uint64_t total_us = (uint64_t)rtc_config.slotElapsedUs + elapsed_us;
    uint32_t slots    = (uint32_t)(total_us / SLOT_US);
    rtc_config.slotElapsedUs = (uint32_t)(total_us % SLOT_US);

    if (slots) {
        uint32_t abs_slot = (uint32_t)rtc_config.currentSlot + slots;
        rtc_config.currentSlot = (uint8_t)(abs_slot % SLOTS_PER_DAY);
        ESP_LOGI(TAG, "Slot advanced -> %u (+%lu ms into slot)",
                 rtc_config.currentSlot,
                 (unsigned long)(rtc_config.slotElapsedUs / 1000UL));

        /* Every complete lap of the time axis is one day. */
        for (uint32_t lap = abs_slot / SLOTS_PER_DAY; lap > 0; lap--) {
            config_slot_day_bump();
        }
    }
}

void config_advance_slot(uint32_t elapsed_s)
{
    config_advance_slot_us((uint64_t)elapsed_s * 1000000ULL);
}

uint8_t config_get_slot(void)
{
    return rtc_config.currentSlot;
}

bool config_slot_valid(void)
{
    return rtc_config.slotValid;
}

uint32_t config_slot_elapsed_us(void)
{
    return rtc_config.slotElapsedUs;
}

int32_t config_get_slot_error_us(void)
{
    return rtc_config.slotErrorUs;
}

/* ──────────── Delta Threshold Checks ──────────── */

static int16_t delta_abs_diff(int16_t a, int16_t b)
{
    int16_t d = a - b;
    return (d < 0) ? -d : d;
}

bool node_check_delta(const sensor_data_t *data)
{
    app_config_t *cfg = &rtc_config;
    int8_t temp_int = (int8_t)(data->temperature * 10.0f + 0.5f);

    /* First send ever for a sensor → report */
    if (cfg->lastSentTemp == INT8_MIN) return true;

    /* Temperature delta check: stored as °C × 10 */
    if (delta_abs_diff(temp_int, cfg->lastSentTemp) >= cfg->deltaTemp) {
        ESP_LOGI("CONFIG", "Delta[temp]: %d -> %d (Δ≥%d), reporting",
                 cfg->lastSentTemp, temp_int, cfg->deltaTemp);
        return true;
    }

    /* Humidity delta check */
    uint8_t hum = (uint8_t)(data->humidity + 0.5f);
    if (delta_abs_diff(hum, cfg->lastSentHumidity) >= cfg->deltaHumidity) {
        ESP_LOGI("CONFIG", "Delta[humidity]: %d%% -> %d%% (Δ≥%d), reporting",
                 cfg->lastSentHumidity, hum, cfg->deltaHumidity);
        return true;
    }

    /* Soil delta check (even though absolute threshold is checked first) */
    if (delta_abs_diff(data->soil_moisture_pct, cfg->lastSentSoil) >= cfg->deltaSoil) {
        ESP_LOGI("CONFIG", "Delta[soil]: %d%% -> %d%% (Δ≥%d), reporting",
                 cfg->lastSentSoil, data->soil_moisture_pct, cfg->deltaSoil);
        return true;
    }

    /* Battery delta check (slow-changing, wide threshold) */
    if (data->battery <= 100) {  /* 0xFF = external power, skip check */
        if (delta_abs_diff(data->battery, cfg->lastSentBattery) >= cfg->deltaBattery) {
            ESP_LOGI("CONFIG", "Delta[battery]: %d%% -> %d%% (Δ≥%d), reporting",
                     cfg->lastSentBattery, data->battery, cfg->deltaBattery);
            return true;
        }
    }

    ESP_LOGD("CONFIG", "No delta change significant enough to report");
    return false;
}

uint8_t node_check_delta_fields(const sensor_data_t *data)
{
    app_config_t *cfg = &rtc_config;
    int8_t temp_int = (int8_t)(data->temperature * 10.0f + 0.5f);
    uint8_t hum = (uint8_t)(data->humidity + 0.5f);
    uint8_t presence = 0;

    /* First send ever → report everything */
    if (cfg->lastSentTemp == INT8_MIN) return PRESENCE_ALL;

    /* Temperature delta */
    if (delta_abs_diff(temp_int, cfg->lastSentTemp) >= cfg->deltaTemp) {
        ESP_LOGI("CONFIG", "Delta[temp]: %d -> %d (Δ≥%d), including",
                 cfg->lastSentTemp, temp_int, cfg->deltaTemp);
        presence |= PRESENCE_TEMP;
    }

    /* Humidity delta */
    if (delta_abs_diff(hum, cfg->lastSentHumidity) >= cfg->deltaHumidity) {
        ESP_LOGI("CONFIG", "Delta[humidity]: %d%% -> %d%% (Δ≥%d), including",
                 cfg->lastSentHumidity, hum, cfg->deltaHumidity);
        presence |= PRESENCE_HUM;
    }

    /* Soil delta */
    if (delta_abs_diff(data->soil_moisture_pct, cfg->lastSentSoil) >= cfg->deltaSoil) {
        ESP_LOGI("CONFIG", "Delta[soil]: %d%% -> %d%% (Δ≥%d), including",
                 cfg->lastSentSoil, data->soil_moisture_pct, cfg->deltaSoil);
        presence |= PRESENCE_SOIL;
    }

    /* Battery delta (skip if 0xFF = external power) */
    if (data->battery <= 100) {
        if (delta_abs_diff(data->battery, cfg->lastSentBattery) >= cfg->deltaBattery) {
            ESP_LOGI("CONFIG", "Delta[battery]: %d%% -> %d%% (Δ≥%d), including",
                     cfg->lastSentBattery, data->battery, cfg->deltaBattery);
            presence |= PRESENCE_BATTERY;
        }
    }

    /* Build current flags and check for change */
    uint8_t cur_flags = 0;
    if (cfg->pumpState)        cur_flags |= FLAG_PUMP_STATE;
    if (cfg->thresholdExceeded) cur_flags |= FLAG_THRESHOLD_EXCEEDED;
    if (cfg->gatewayLost)      cur_flags |= FLAG_GATEWAY_LOST;
    if ((data->sensor_error) == 0) cur_flags |= FLAG_SENSOR_OK;
    if (cur_flags != cfg->lastSentFlags) {
        ESP_LOGI("CONFIG", "Delta[flags]: 0x%02X -> 0x%02X, including",
                 cfg->lastSentFlags, cur_flags);
        presence |= PRESENCE_FLAGS;
    }

    return presence;
}

void node_store_last_sent(const sensor_data_t *data)
{
    app_config_t *cfg = &rtc_config;
    cfg->lastSentTemp = (int8_t)(data->temperature * 10.0f + 0.5f);
    cfg->lastSentHumidity = (uint8_t)(data->humidity + 0.5f);
    cfg->lastSentSoil = data->soil_moisture_pct;
    cfg->lastSentBattery = data->battery;

    /* Build flags the same way commands_build_data_packet does */
    uint8_t flags = 0;
    if (rtc_config.pumpState)        flags |= FLAG_PUMP_STATE;
    if (rtc_config.thresholdExceeded) flags |= FLAG_THRESHOLD_EXCEEDED;
    if (rtc_config.gatewayLost)      flags |= FLAG_GATEWAY_LOST;
    if ((data->sensor_error) == 0)    flags |= FLAG_SENSOR_OK;
    cfg->lastSentFlags = flags;
}

void node_store_last_sent_compact(const sensor_data_t *data, uint8_t presence)
{
    app_config_t *cfg = &rtc_config;
    int8_t temp_int = (int8_t)(data->temperature * 10.0f + 0.5f);
    uint8_t hum = (uint8_t)(data->humidity + 0.5f);

    if (presence & PRESENCE_TEMP)    cfg->lastSentTemp = temp_int;
    if (presence & PRESENCE_HUM)     cfg->lastSentHumidity = hum;
    if (presence & PRESENCE_SOIL)    cfg->lastSentSoil = data->soil_moisture_pct;
    if (presence & PRESENCE_BATTERY) cfg->lastSentBattery = data->battery;
    if (presence & PRESENCE_FLAGS) {
        uint8_t flags = 0;
        if (rtc_config.pumpState)        flags |= FLAG_PUMP_STATE;
        if (rtc_config.thresholdExceeded) flags |= FLAG_THRESHOLD_EXCEEDED;
        if (rtc_config.gatewayLost)      flags |= FLAG_GATEWAY_LOST;
        if ((data->sensor_error) == 0)    flags |= FLAG_SENSOR_OK;
        cfg->lastSentFlags = flags;
    }
}

/* ──────────── Configurable Delta Thresholds (get/set) ──────────── */

uint8_t config_get_delta_temp(void)    { return rtc_config.deltaTemp; }
uint8_t config_get_delta_humidity(void) { return rtc_config.deltaHumidity; }
uint8_t config_get_delta_soil(void)    { return rtc_config.deltaSoil; }
uint8_t config_get_delta_battery(void) { return rtc_config.deltaBattery; }

void config_set_delta_temp(uint8_t val)
{
    if (val < 5) val = 5;          /* Minimum 0.5°C */
    if (val > 100) val = 100;      /* Maximum 10.0°C */
    rtc_config.deltaTemp = val;
    ESP_LOGI(TAG, "Delta threshold[temp] set to: %d (%.1f°C)", val, val / 10.0f);
}

void config_set_delta_humidity(uint8_t val)
{
    if (val < 1) val = 1;
    if (val > 50) val = 50;
    rtc_config.deltaHumidity = val;
    ESP_LOGI(TAG, "Delta threshold[humidity] set to: %d%%", val);
}

void config_set_delta_soil(uint8_t val)
{
    if (val < 1) val = 1;
    if (val > 50) val = 50;
    rtc_config.deltaSoil = val;
    ESP_LOGI(TAG, "Delta threshold[soil] set to: %d%%", val);
}

void config_set_delta_battery(uint8_t val)
{
    if (val < 1) val = 1;
    if (val > 50) val = 50;
    rtc_config.deltaBattery = val;
    ESP_LOGI(TAG, "Delta threshold[battery] set to: %d%%", val);
}

/* ──────────── NVS Persistence ──────────── */

#define NVS_THR_LOW_KEY   "thr_low"
#define NVS_THR_HIGH_KEY  "thr_high"
#define NVS_DT_TEMP_KEY   "dt_temp"
#define NVS_DT_HUM_KEY    "dt_hum"
#define NVS_DT_SOIL_KEY   "dt_soil"
#define NVS_DT_BAT_KEY    "dt_bat"

void save_threshold_to_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("node", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open for write failed: %s", esp_err_to_name(err));
        return;
    }

    nvs_set_u8(handle, NVS_THR_LOW_KEY, rtc_config.thresholdLow);
    nvs_set_u8(handle, NVS_THR_HIGH_KEY, rtc_config.thresholdHigh);
    nvs_set_u8(handle, NVS_DT_TEMP_KEY, rtc_config.deltaTemp);
    nvs_set_u8(handle, NVS_DT_HUM_KEY, rtc_config.deltaHumidity);
    nvs_set_u8(handle, NVS_DT_SOIL_KEY, rtc_config.deltaSoil);
    nvs_set_u8(handle, NVS_DT_BAT_KEY, rtc_config.deltaBattery);

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Saved to NVS: soil=[%u-%u] delta(T=%u H=%u S=%u B=%u%%)",
                 rtc_config.thresholdLow, rtc_config.thresholdHigh,
                 rtc_config.deltaTemp, rtc_config.deltaHumidity,
                 rtc_config.deltaSoil, rtc_config.deltaBattery);
    }

    nvs_close(handle);
}

void load_threshold_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("node", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "NVS open for read failed (first boot?): %s", esp_err_to_name(err));
        return;
    }

    uint8_t val;
    err = nvs_get_u8(handle, NVS_THR_LOW_KEY, &val);
    if (err == ESP_OK) rtc_config.thresholdLow = val;

    err = nvs_get_u8(handle, NVS_THR_HIGH_KEY, &val);
    if (err == ESP_OK) rtc_config.thresholdHigh = val;

    err = nvs_get_u8(handle, NVS_DT_TEMP_KEY, &val);
    if (err == ESP_OK) rtc_config.deltaTemp = val;

    err = nvs_get_u8(handle, NVS_DT_HUM_KEY, &val);
    if (err == ESP_OK) rtc_config.deltaHumidity = val;

    err = nvs_get_u8(handle, NVS_DT_SOIL_KEY, &val);
    if (err == ESP_OK) rtc_config.deltaSoil = val;

    err = nvs_get_u8(handle, NVS_DT_BAT_KEY, &val);
    if (err == ESP_OK) rtc_config.deltaBattery = val;

    nvs_close(handle);

    ESP_LOGI(TAG, "Loaded from NVS: soil=[%u-%u] delta(T=%u H=%u S=%u B=%u%%)",
             rtc_config.thresholdLow, rtc_config.thresholdHigh,
             rtc_config.deltaTemp, rtc_config.deltaHumidity,
             rtc_config.deltaSoil, rtc_config.deltaBattery);
}

/* Override config_set_thresholds to also persist to NVS */
void config_set_thresholds(uint8_t low, uint8_t high)
{
    if (low >= high) {
        ESP_LOGW(TAG, "Invalid thresholds: low=%u >= high=%u — rejecting", low, high);
        return;
    }
    rtc_config.thresholdLow = low;
    rtc_config.thresholdHigh = high;
    ESP_LOGI(TAG, "Thresholds set to: Low=%u%%, High=%u%%", low, high);
    save_threshold_to_nvs();
}

/* ──────────── Schedule state persistence (NVS) ────────────
 *
 * The daily schedule is driven by the slot-day counter on the 15-minute time
 * axis. Persisting day + last-watered-day keeps "once per day" valid across
 * resets and power loss (on a reset the RTC copy is gone, the NVS copy is not). */
#define NVS_SLOT_DAY_KEY   "sch_day"
#define NVS_LAST_WATER_KEY "sch_last"

void save_schedule_state_to_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("node", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open for schedule write failed: %s", esp_err_to_name(err));
        return;
    }

    nvs_set_u16(handle, NVS_SLOT_DAY_KEY, rtc_config.slotDay);
    nvs_set_u16(handle, NVS_LAST_WATER_KEY, rtc_config.lastWateringDay);

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit (schedule) failed: %s", esp_err_to_name(err));
    }
    nvs_close(handle);
}

void load_schedule_state_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("node", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "NVS open for schedule read failed (first boot?): %s",
                 esp_err_to_name(err));
        return;
    }

    uint16_t day = 0, last = 0xFFFF;
    bool have_day  = (nvs_get_u16(handle, NVS_SLOT_DAY_KEY, &day) == ESP_OK);
    bool have_last = (nvs_get_u16(handle, NVS_LAST_WATER_KEY, &last) == ESP_OK);
    nvs_close(handle);

    if (have_day)  rtc_config.slotDay = day;
    if (have_last) rtc_config.lastWateringDay = last;

    if (have_day || have_last) {
        ESP_LOGI(TAG, "Loaded schedule state from NVS: day=%u lastWateredDay=%u",
                 rtc_config.slotDay, rtc_config.lastWateringDay);
    }
}

/* ──────────── Alarm state persistence (NVS) ────────────
 *
 * Save the alarm snapshot (persistedAlarmCode) and the last-reported code so
 * a cold boot / power loss can restore what the gateway already knows.
 * IMPORTANT: this needs NVS initialised. It is called from app_main after
 * nvs_flash_init (config.c functions only; see main.c Step 0/3). */
#define NVS_ALARM_PERSIST_KEY "alarm_persist"
#define NVS_ALARM_LAST_KEY    "alarm_last"

void save_alarm_state_to_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("node", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open for alarm write failed: %s", esp_err_to_name(err));
        return;
    }

    nvs_set_u8(handle, NVS_ALARM_PERSIST_KEY, rtc_config.persistedAlarmCode);
    nvs_set_u8(handle, NVS_ALARM_LAST_KEY,    rtc_config.lastAlarmCode);
    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit alarm state failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Alarm state saved: persist=0x%02X last=0x%02X",
                 rtc_config.persistedAlarmCode, rtc_config.lastAlarmCode);
    }
    nvs_close(handle);
}

void load_alarm_state_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("node", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "NVS open for alarm read failed: %s", esp_err_to_name(err));
        return;
    }

    uint8_t val = 0;
    if (nvs_get_u8(handle, NVS_ALARM_PERSIST_KEY, &val) == ESP_OK) {
        rtc_config.persistedAlarmCode = val;
    }
    if (nvs_get_u8(handle, NVS_ALARM_LAST_KEY, &val) == ESP_OK) {
        rtc_config.lastAlarmCode = val;
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "Alarm state loaded: persist=0x%02X last=0x%02X",
             rtc_config.persistedAlarmCode, rtc_config.lastAlarmCode);
}
