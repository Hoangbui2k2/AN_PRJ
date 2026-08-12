/**
 * @file main.c
 * @brief ESP32 LoRa Irrigation Node - Main Application Entry
 *
 * System overview:
 * - UART LoRa (SX1278 module) on UART2 for wireless communication
 * - DHT22 temperature/humidity + capacitive soil moisture sensor
 * - MOSFET-switched sensor power for low deep-sleep current
 * - CD4013 toggle-based pump relay control
 * - Deep-sleep with combined timer + EXT0 wake on button GPIO34
 * - Three operation modes: Manual, Schedule, Threshold
 * - Alarm LED with blink-code patterns and alarm packets
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_timer.h"

#include "config.h"
#include "crc.h"
#include "lora_uart.h"
#include "sensors.h"
#include "pump.h"
#include "power.h"
#include "alarms.h"
#include "irrigation.h"
#include "commands.h"

/* Boot LED flash utility */
static void boot_led_flash(void)
{
    /* Flash alarm LED 3 times to indicate successful boot */
    for (int i = 0; i < 3; i++) {
        alarm_led_on();
        vTaskDelay(pdMS_TO_TICKS(100));
        alarm_led_off();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static const char *TAG = "MAIN";

/* ──────────── Alarm Detection ──────────── */
static void check_and_update_alarms(const sensor_data_t *data)
{
    app_config_t *cfg = config_get();

    /* 1. Sensor Error: 3 consecutive failures */
    if (data->sensor_error != 0) {
        config_increment_sensor_errors();
        if (cfg->sensorErrorCount >= 3) {
            config_set_alarm(ALARM_SENSOR_ERROR);
        }
    } else {
        config_reset_sensor_errors();
    }

    /* 2. Soil out of range: soil < 10% or > 90% (only if soil sensor read succeeded) */
    if ((data->sensor_error & 0x02) == 0) {
        if (data->soil_moisture_pct < 10 || data->soil_moisture_pct > 90) {
            config_set_alarm(ALARM_SOIL_OUT_RANGE);
        }
    }

    /* 3. Low Battery: battery < 20% (our battery returns 0xFF on external boost MT3608, so ignored) */
    if (data->battery < 20) {
        config_set_alarm(ALARM_LOW_BATTERY);
    }

    /* 4. Gateway Lost: detected in the heartbeat path (commands.c, spec C).
     * Alarm 0x05 is raised exactly when gatewayLostCount first reaches
     * GW_LOST_ALARM_THRESHOLD, so it is not duplicated here. */

    /* 5. Edge-triggered clearing: if a sensor-related alarm's condition is no
     * longer met, clear it so the edge-detect in Step 11 can send the 0x00
     * "alarm cleared" packet. RELAY_ERROR and GATEWAY_LOST are latched from
     * other paths and cleared there (pump success / valid downlink). */
    if (cfg->alarmCode == ALARM_SENSOR_ERROR && data->sensor_error == 0) {
        config_clear_alarm();
    } else if (cfg->alarmCode == ALARM_SOIL_OUT_RANGE &&
               (data->sensor_error & 0x02) == 0 &&
               data->soil_moisture_pct >= 10 && data->soil_moisture_pct <= 90) {
        config_clear_alarm();
    } else if (cfg->alarmCode == ALARM_LOW_BATTERY && data->battery >= 20) {
        config_clear_alarm();
    }
}

/**
 * @brief Reconcile an alarm that was active before a power loss / cold boot
 *
 * After reset, config_load_alarm_state() restores persistedAlarmCode (the
 * alarm that was active when it was raised, saved to NVS) and lastAlarmCode
 * (what the gateway was last told). sensor_data has just been read, so we can
 * decide how to resume:
 *
 *   - Alarm still active (sensor OK + condition true)  → gateway already knows,
 *     do nothing.
 *   - Alarm truly cleared (sensor OK + condition gone) → tell the gateway by
 *     sending code 0x00.
 *   - Cannot tell (sensor read error / unknown)        → re-send the persisted
 *     active alarm so the gateway knows status is uncertain.
 *
 * Runs only on the cycle immediately after boot (persistedAlarmCode != NONE),
 * then resets the snapshot so later cycles use normal edge detection.
 */
static void handle_boot_alarm_recovery(const sensor_data_t *data)
{
    app_config_t *cfg = config_get();
    uint8_t persisted = config_get_persisted_alarm();
    if (persisted == ALARM_NONE) {
        return;   /* nothing to restore — normal wake cycle */
    }

    /* cur is cfg->alarmCode after check_and_update_alarms ran this cycle.
     * If a condition is still true, it equals persisted; if cleared, it is
     * ALARM_NONE; if sensor failed, it may be NONE too. */
    uint8_t cur = cfg->alarmCode;
    bool certain = (data->sensor_error == 0);

    /* Only sensor-driven alarms can be verified as "cleared" after boot.
     * RELAY_ERROR / GATEWAY_LOST have no sensor condition to check, so they
     * are always treated as still-present → re-send active code. */
    bool can_verify_clear =
        (persisted == ALARM_SENSOR_ERROR ||
         persisted == ALARM_SOIL_OUT_RANGE ||
         persisted == ALARM_LOW_BATTERY);

    if (certain && can_verify_clear) {
        if (cur == persisted) {
            ESP_LOGI(TAG, "Boot recovery: alarm 0x%02X still active — no re-send",
                     persisted);
            /* Keep lastAlarmCode in sync with the known-active code so the
             * edge-detect in Step 11 sees cur == last and does NOT re-send. */
            config_set_last_alarm(persisted);
        } else {
            ESP_LOGI(TAG, "Boot recovery: alarm 0x%02X cleared — sending 0x00",
                     persisted);
            commands_send_alarm(cfg->nodeId, ALARM_NONE, data);
            config_set_last_alarm(ALARM_NONE);
        }
    } else {
        /* Either sensors unknown after boot, or the persisted alarm type
         * cannot be verified (relay / gateway-lost). Re-send the active code
         * so the gateway knows the alarm (likely) still exists. */
        ESP_LOGI(TAG, "Boot recovery: cannot verify (%scertain, code 0x%02X), "
                 "re-sending alarm 0x%02X",
                 certain ? "" : "not-", persisted, persisted);
        commands_send_alarm(cfg->nodeId, persisted, data);
        config_set_last_alarm(persisted);
        /* Keep alarmCode in sync so Step 11 edge-detect (cur vs last) does not
         * re-transmit the same alarm again. */
        cfg->alarmCode = persisted;
    }

    /* Snapshot consumed — clear so later cycles use normal edge-detect. */
    config_set_persisted_alarm(ALARM_NONE);
}

/* ──────────── Application Entry ──────────── */

void app_main(void)
{
    /* ── Step 0: Boot LED flash ── */
    boot_led_flash();

    /* ── Step 1: Initialize NVS (needed by IDF internal subsystems) ── */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* ── Step 2: Determine wake source ── */
    esp_sleep_wakeup_cause_t wake_cause = power_get_wake_cause();
    bool button_wake = (wake_cause == ESP_SLEEP_WAKEUP_EXT0);
    bool timer_wake  = (wake_cause == ESP_SLEEP_WAKEUP_TIMER);

    if (wake_cause == ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "Cold boot (first power-on)");
    } else if (button_wake) {
        ESP_LOGI(TAG, "Woke from button press (GPIO34)");
    } else if (timer_wake) {
        ESP_LOGI(TAG, "Woke from timer");
    } else {
        ESP_LOGI(TAG, "Woke from unknown cause: %d", wake_cause);
    }

    /* ── Step 3: Initialize all subsystems ── */
    config_load();           /* Load RTC config (may init defaults) */

    /* Load persisted thresholds from NVS (overrides defaults if present) */
    load_threshold_from_nvs();

    /* Restore alarm snapshot + last-reported code from NVS so a cold boot /
     * power loss remembers whether the gateway already knows an alarm. On a
     * deep-sleep wake these values equal the RTC ones (harmless reload). */
    load_alarm_state_from_nvs();
    power_init();            /* MOSFET sensor power and Wake Button */
    sensors_init();          /* DHT22 and Soil Moisture ADC */
    pump_init();             /* CD4013 clock toggle pin */
    alarms_init();           /* Alarm LED pin */
    lora_uart_init();        /* UART2 and control pins */
    irrigation_init();       /* Irrigation state variables */

    app_config_t *cfg = config_get();

    power_sensor_on();
    /* ── Step 4: Handle wake causes ── */

    if (button_wake) {
        /* Brief LED flash to acknowledge button press */
        alarm_led_on();
        vTaskDelay(pdMS_TO_TICKS(100));
        alarm_led_off();

        /* Debounce: wait 50 ms then re-check the button level.
         * This filters out switch bounce and ensures the press is genuine. */
        vTaskDelay(pdMS_TO_TICKS(50));
        if (gpio_get_level(BUTTON_GPIO) != 0) {
            ESP_LOGW(TAG, "Button wake but level returned HIGH after debounce — ignoring");
            power_deep_sleep();
        }

        ESP_LOGI(TAG, "Button wake - toggling pump");
        pump_toggle();

        /* Read sensors and transmit status update so gateway receives immediate pump state change */
        // power_sensor_on();
        // vTaskDelay(pdMS_TO_TICKS(7000));
        sensor_data_t sensor_data;
        sensors_read(&sensor_data);
        // vTaskDelay(pdMS_TO_TICKS(2000)); /* Allow sensors to stabilize */

        // power_sensor_off();

        ESP_LOGI(TAG, "Transmitting updated status after button toggle");
        commands_send_data_with_ack(cfg->nodeId, &sensor_data);
        reset_cycle_counter();  /* Reset heartbeat counter on button-triggered send */

        /* Wait briefly for any immediate configuration downlink from gateway */
        commands_check_pending(&sensor_data);

        ESP_LOGI(TAG, "Button cycle complete - entering deep sleep");
        lora_sleep();          /* needed before deep sleep */
        power_deep_sleep();
        /* never reaches here */
    }

    /* ── Timer Wake or Cold Boot cycle ── */

    /* Step 5: Power sensors via MOSFET and read data */
    // power_sensor_on();
    // vTaskDelay(pdMS_TO_TICKS(7000)); /* Allow sensors to stabilize */

    sensor_data_t sensor_data;
    sensors_read(&sensor_data);
    // vTaskDelay(pdMS_TO_TICKS(2000)); /* Allow sensors to stabilize */
    // power_sensor_off();

    /* Step 6: Perform Alarm Checks */
    check_and_update_alarms(&sensor_data);

    /* Step 6b: Reconcile an alarm that survived a power loss / cold boot
     * (persisted snapshot). Runs once; normal edge-detect resumes after. */
    handle_boot_alarm_recovery(&sensor_data);

    /* Step 7: Check for commands prior to irrigation execution */
    commands_check_pending(&sensor_data);

    /* Step 8: Run irrigation controller cycle */
    irrigation_cycle(&sensor_data);

    /* Step 9: Decide what/whether to send to gateway
     *
     * Priority:
     * 1. First send ever                   → send full 0x01 (gateway baseline)
     * 2. Soil exceeds absolute threshold   → compact 0x06 with soil + delta-changed
     *                                        fields, ONLY when something changed
     *                                        (no repeat on unchanged value)
     * 3. Sensor delta detected             → compact 0x06 (only changed fields)
     * 4. Periodic heartbeat due            → heartbeat 0x02 (flags+battery only)
     * 5. Otherwise                         → skip (power saving — no LoRa TX)
     */
    uint8_t delta = node_check_delta_fields(&sensor_data);
    bool exceeded = node_check_threshold(sensor_data.soil_moisture_pct);
    bool data_heartbeat = is_heartbeat_time();

    /* DEBUG: which branch will win in the send decision below */
    ESP_LOGI(TAG, "DBG send-decision: lastSent=%d delta=0x%02X exceeded=%d "
             "hb=%d cyclesSinceSend=%d hb_cycles=%d",
             cfg->lastSentTemp, delta, exceeded, data_heartbeat,
             cfg->cyclesSinceSend, HEARTBEAT_CYCLES);

    if (cfg->lastSentTemp == INT8_MIN) {
        /* First send since cold boot — send full 0x01 so gateway has a baseline */
        ESP_LOGI(TAG, "First send — sending full baseline packet");
        commands_send_data_with_ack(cfg->nodeId, &sensor_data);
        node_store_last_sent(&sensor_data);
        reset_cycle_counter();
    } else if (exceeded) {
        /* Soil out of absolute range.  Only re-send when the soil value or the
         * threshold-exceeded flag actually changed (delta) — otherwise skip so
         * the SAME soil value is not re-transmitted on every wake cycle.  The
         * packet carries soil (the trigger) plus any other delta-changed fields. */
        uint8_t presence = delta | PRESENCE_SOIL;
        if (delta != 0) {
            ESP_LOGI(TAG, "Soil %d%% exceeds threshold [%d-%d] — sending update (presence 0x%02X)",
                     sensor_data.soil_moisture_pct, cfg->thresholdLow, cfg->thresholdHigh, presence);
            if (commands_send_compact_with_ack(cfg->nodeId, &sensor_data, presence) == 0) {
                node_store_last_sent_compact(&sensor_data, presence);
            }
            reset_cycle_counter();
        } else if (data_heartbeat) {
            /* Soil still out of range but unchanged: don't re-send the same
             * soil value, but DO send the liveness heartbeat when one is due.
             * commands_build_heartbeat_packet still carries
             * FLAG_THRESHOLD_EXCEEDED (soil out of range) in its flags byte,
             * so the gateway sees both liveness and the exceeded state. */
            ESP_LOGI(TAG, "Soil still %d%% out of range, unchanged — sending heartbeat (0x02)",
                     sensor_data.soil_moisture_pct);
            if (commands_send_heartbeat(cfg->nodeId, sensor_data.battery, &sensor_data) == 0) {
                /* Update lastSentSoil too so node_check_delta_fields stops
                 * reporting PRESENCE_SOIL every cycle (the heartbeat itself
                 * does not carry soil). */
                node_store_last_sent_compact(&sensor_data,
                    PRESENCE_SOIL | PRESENCE_FLAGS | PRESENCE_BATTERY);
            }
            reset_cycle_counter();
        } else {
            ESP_LOGI(TAG, "Soil still %d%% exceeds threshold — skipping send "
                     "(resend on next soil delta)",
                     sensor_data.soil_moisture_pct);
        }
    } else if (delta) {
        ESP_LOGI(TAG, "Sensor change 0x%02X — sending compact update", delta);
        if (commands_send_compact_with_ack(cfg->nodeId, &sensor_data, delta) == 0) {
            node_store_last_sent_compact(&sensor_data, delta);
        }
        reset_cycle_counter();
    } else if (data_heartbeat) {
        ESP_LOGI(TAG, "Within threshold, no delta — sending heartbeat (0x02)");
        if (commands_send_heartbeat(cfg->nodeId, sensor_data.battery, &sensor_data) == 0) {
            node_store_last_sent_compact(&sensor_data, PRESENCE_FLAGS | PRESENCE_BATTERY);
        }
        reset_cycle_counter();
    } else {
        ESP_LOGI(TAG, "All sensors stable — skipping send (cycle %d)",
                 cfg->cyclesSinceSend + 1);
    }
    increment_cycle_counter();

    /* Step 10: Check for commands after data send (downlink window) */
    commands_check_pending(&sensor_data);

    /* Step 11: Edge-triggered alarm reporting — send ONE packet when an alarm
     * starts (NONE -> code) and ONE packet with code 0x00 when it clears
     * (code -> NONE). Do NOT re-send while the same alarm stays active. */
    uint8_t cur_alarm = config_get_alarm();
    uint8_t prev_alarm = config_get_last_alarm();

    if (cur_alarm != prev_alarm) {
        if (cur_alarm != ALARM_NONE) {
            /* New alarm appeared */
            ESP_LOGW(TAG, "Alarm START 0x%02X (was 0x%02X) -> sending", cur_alarm, prev_alarm);
            commands_send_alarm(cfg->nodeId, cur_alarm, &sensor_data);
            alarms_signal((alarm_type_t)cur_alarm);
        } else {
            /* Alarm cleared */
            ESP_LOGI(TAG, "Alarm CLEARED (was 0x%02X) -> sending 0x00", prev_alarm);
            commands_send_alarm(cfg->nodeId, ALARM_NONE, &sensor_data);
        }
        config_set_last_alarm(cur_alarm);
        /* Do NOT clear cfg->alarmCode here: it latches the current active
         * alarm. It is only zeroed when conditions actually clear (see
         * check_and_update_alarms) or when a downlink clears it. */
    }

    /* Step 12: Verify CD4013 GPIO state */
    pump_verify_state();

    /* Step 13: Put LoRa module to sleep */
    lora_sleep();

    /* Step 14: Log cycle summary */
    ESP_LOGI(TAG, "=== Cycle Complete ===");
    ESP_LOGI(TAG, "  Mode:          %s",
             cfg->mode == MODE_MANUAL   ? "Manual" :
             cfg->mode == MODE_SCHEDULE ? "Schedule" :
             cfg->mode == MODE_THRESHOLD ? "Threshold" : "Unknown");
    ESP_LOGI(TAG, "  Pump:          %s", cfg->pumpState ? "ON" : "OFF");
    ESP_LOGI(TAG, "  Temp:          %.1f C", sensor_data.temperature);
    ESP_LOGI(TAG, "  Humidity:      %.1f %%", sensor_data.humidity);
    ESP_LOGI(TAG, "  Soil Moisture: %d%% (raw: %d)", sensor_data.soil_moisture_pct, sensor_data.soil_moisture_raw);
    ESP_LOGI(TAG, "  Threshold:     [%d-%d] %s",
             cfg->thresholdLow, cfg->thresholdHigh,
             cfg->thresholdExceeded ? "EXCEEDED" : "OK");
    ESP_LOGI(TAG, "  Cycles/Send:   %d", cfg->cyclesSinceSend);
    ESP_LOGI(TAG, "  Failed ACKs:   %d", cfg->gatewayLostCount);
    ESP_LOGI(TAG, "  Alarm:         0x%02X", cfg->alarmCode);
    ESP_LOGI(TAG, "  Sleep Time:    %u s", cfg->interval);
    ESP_LOGI(TAG, "  Pump Cycles:   %lu", (unsigned long)cfg->totalPumpCycles);
    ESP_LOGI(TAG, "========================");

    /* Step 15: Enter deep sleep */
    power_deep_sleep();
}
