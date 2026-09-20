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
#include "esp_private/esp_clk.h"  /* esp_clk_rtc_time() — RTC clock (µs), alive across deep sleep */

#include "config.h"
#include "crc.h"
#include "lora_uart.h"
#include "sensors.h"
#include "pump.h"
#include "power.h"
#include "alarms.h"
#include "irrigation.h"
#include "commands.h"
#include "baseline.h"

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

/* Button behaviour while a timed run (schedule / relay-ON duration) is active:
 *   0 = PROTECT the run — a press only wakes the node, reports status and
 *       re-sleeps for the remaining run time (the watering is not cut short).
 *   1 = let the press toggle the pump (explicit manual override, ends the run).
 * Manual control from the gateway (0x02 / 0x03 / 0x07) always ends the run. */
#define BUTTON_OVERRIDES_TIMED_RUN  0

/* ──────────── Timekeeping instrumentation (awake vs sleep) ────────────
 *
 * The node runs ONE sequential cycle per wake: awake → deep sleep → awake …
 * Both halves must be measured for the 15-minute time axis (t) to be trusted:
 *
 *   awake : esp_timer_get_time() delta inside a single boot. It restarts at
 *           every wake, so it can never measure the sleep part.
 *   sleep : esp_clk_rtc_time() (the RTC counter accumulated in RTC-retained
 *           memory) keeps counting through deep sleep.
 *           A delta between the same point of two consecutive cycles is
 *           therefore one WHOLE real cycle = awake tail + sleep + boot
 *           overhead + awake head.
 *
 * The slot estimate is advanced by that real cycle delta (see Step 4c), so the
 * boot overhead and the awake time — which the old `config_advance_slot(
 * cfg->interval)` silently dropped — are now accounted for. Two independent
 * drift checks are logged:
 *   - per-cycle: real vs. nominal interval, plus the running total;
 *   - per-resync: the local estimate's error vs. the gateway's slot
 *     (config_get_slot_error_us()).
 */

#define NODE_TIME_MAX_DELTA_US  ((uint64_t)SLOT_US * SLOTS_PER_DAY)  /* 1 day */

/* Measurement state kept in RTC memory (survives deep sleep) */
RTC_DATA_ATTR static uint64_t s_rtc_mark_us    = 0; /* RTC time at Step 4c        */
RTC_DATA_ATTR static uint64_t s_rtc_sleep_us   = 0; /* RTC time before sleeping   */
RTC_DATA_ATTR static uint64_t s_awake_prev_us  = 0; /* previous awake duration    */
RTC_DATA_ATTR static uint64_t s_cycles_timed   = 0; /* cycles measured since sync */
RTC_DATA_ATTR static int64_t  s_drift_total_us = 0; /* Σ(real cycle − interval)   */

/**
 * @brief Record the awake duration of this cycle and the RTC mark right before
 *        the node goes to sleep (used by the next cycle to compute how long the
 *        node was actually asleep + the boot overhead).
 */
static void node_time_mark_sleep(uint64_t t_boot_us)
{
    s_awake_prev_us = (uint64_t)esp_timer_get_time() - t_boot_us;
    s_rtc_sleep_us  = esp_clk_rtc_time();
    ESP_LOGI(TAG, "TIME: awake %llu ms (this cycle)",
             (unsigned long long)(s_awake_prev_us / 1000ULL));
}

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

/* ──────────── Boot-time baseline / time sync ──────────── */

#define BOOT_SYNC_LISTEN_MS   30000UL                 /* per-attempt listen window */
#define BOOT_SYNC_CAP_MS      (15UL * 60UL * 1000UL)  /* baseline safety cap (~15 min) */
#define BOOT_SYNC_TIME_TAIL_MS  6000UL                /* cửa sổ xin time sau khi xong baseline */

/**
 * @brief If the reassembly session is complete, persist it and tell the
 *        gateway (uplink 0x08) which series were stored.
 */
static void node_finish_baseline_if_ready(void)
{
    /* Shared with commands_wait_baseline() so the DONE uplinks are emitted the
     * moment the last chunk lands, instead of after the whole listen window. */
    commands_finish_baseline_if_ready();
}

/**
 * @brief Ensure the node has a valid slot (time) and, when missing from NVS,
 *        a baseline. Blocks (no deep sleep) until done, with a safety cap.
 *
 * Two independent needs:
 *   - slot     : lost on any reset that clears RTC memory
 *   - baseline : only when NVS holds no valid series
 */
static void node_boot_sync(void)
{
    bool need_base = (baseline_valid_mask() != 0x07u);
    bool need_time = !config_slot_valid();

    if (!need_base && !need_time) {
        ESP_LOGI(TAG, "Boot sync: slot + baseline present - skipping");
        return;
    }

    ESP_LOGI(TAG, "Boot sync start: need_baseline=%d need_time=%d",
             need_base, need_time);

    /* ── Time only: a single downlink is enough. Try a few times, then carry
     *    on with baseline gating disabled (the slot arrives on the next
     *    uplink's downlink anyway). ── */
    if (!need_base) {
        for (int i = 0; i < 3 && !config_slot_valid(); i++) {
            commands_send_req(REQ_FLAG_TIME, 0);
            commands_wait_baseline(BOOT_SYNC_LISTEN_MS);
        }
        if (config_slot_valid()) {
            ESP_LOGI(TAG, "Boot sync: slot=%u", config_get_slot());
        } else {
            ESP_LOGW(TAG, "Boot sync: no slot yet - baseline gating stays OFF");
        }
        return;
    }

    /* ── Baseline transfer (blocking, bounded by the safety cap) ── */
    baseline_session_start(0x07u, 0);
    uint32_t start_ms = (uint32_t)(esp_timer_get_time() / 1000);

    while (1) {
        uint8_t flags = 0;
        if (!config_slot_valid())           flags |= REQ_FLAG_TIME;
        if (baseline_valid_mask() != 0x07u) flags |= REQ_FLAG_BASELINE;
        if (flags == 0) break;              /* both satisfied */

        commands_send_req(flags, 0x07u);
        commands_wait_baseline(BOOT_SYNC_LISTEN_MS);

        node_finish_baseline_if_ready();

        if ((uint32_t)(esp_timer_get_time() / 1000) - start_ms > BOOT_SYNC_CAP_MS) {
            ESP_LOGW(TAG, "Boot sync cap (%lu s) reached - sleeping 5 min then retrying",
                     (unsigned long)(BOOT_SYNC_CAP_MS / 1000));
            lora_sleep();
            power_deep_sleep();             /* never returns; retried next wake */
        }
    }

    ESP_LOGI(TAG, "Boot sync: baseline xong (0x%02X) - gửi REQ TIME cuối để chốt t",
             baseline_valid_mask());

    /* LUÔN xin time một lần nữa sau khi cấp xong baseline: baseline phải được
     * gắn với đúng t (giờ trong ngày) tại thời điểm cấp, và t được gateway xác
     * nhận ngay trước khi node bắt đầu so sánh nội suy. */
    commands_send_req(REQ_FLAG_TIME, 0);
    commands_wait_baseline(BOOT_SYNC_TIME_TAIL_MS);

    ESP_LOGI(TAG, "Boot sync complete: slot=%u valid=%d baseline=0x%02X",
             config_get_slot(), config_slot_valid(), baseline_valid_mask());
}

/* ──────────── 00:00 boundary parking ──────────── */

#define PARK_MARGIN_S   180UL   /* wake this long before t=0 */
#define SYNC_LISTEN_S   360UL   /* max total listen time for the broadcast sync */

/** Seconds until the next t=0 boundary (0 if the slot is unknown). */
static uint32_t slot_seconds_to_boundary(void)
{
    if (!config_slot_valid()) return 0;

    app_config_t *cfg = config_get();
    uint32_t slots_to_go = (cfg->currentSlot == 0)
                               ? SLOTS_PER_DAY
                               : (SLOTS_PER_DAY - cfg->currentSlot);
    /* µs precision, RTC-measured elapsed time */
    uint64_t remain_us = (uint64_t)slots_to_go * SLOT_US - cfg->slotElapsedUs;
    return (uint32_t)(remain_us / 1000000ULL);
}

/**
 * @brief Park awake shortly before the t=0 boundary to catch the broadcast.
 *
 * If sleeping the normal interval would overshoot midnight, sleep only long
 * enough to wake ~PARK_MARGIN_S before the boundary, then listen for the
 * gateway's t=0 broadcast. Called just before the normal deep sleep.
 */
static void node_park_near_boundary(void)
{
    if (!config_slot_valid()) return;

    app_config_t *cfg = config_get();
    uint32_t secs     = slot_seconds_to_boundary();
    uint32_t park_at  = cfg->interval + PARK_MARGIN_S;

    if (secs == 0 || secs > park_at) return;   /* not close enough yet */

    if (secs > PARK_MARGIN_S) {
        uint32_t nap = secs - PARK_MARGIN_S;
        ESP_LOGI(TAG, "t=0 in %lu s — parking %lu s to wake %lu s before boundary",
                 (unsigned long)secs, (unsigned long)nap,
                 (unsigned long)PARK_MARGIN_S);
        lora_sleep();
        power_deep_sleep_for(nap);            /* never returns */
    }

    ESP_LOGI(TAG, "Near t=0 (%lu s) — listening for broadcast sync",
             (unsigned long)secs);
    lora_wake();
    for (uint32_t waited = 0; waited < SYNC_LISTEN_S * 1000UL; waited += 30000UL) {
        if (config_slot_valid() && config_get_slot() == 0) break;  /* synced */
        commands_wait_baseline(30000UL);
    }
    ESP_LOGI(TAG, "Boundary listen done: slot=%u valid=%d",
             config_get_slot(), config_slot_valid());
}

/* ──────────── Application Entry ──────────── */

void app_main(void)
{
    /* Timekeeping basis, captured as early as possible:
     *  - t_boot_us  : esp_timer (monotonic inside this boot) → awake time
     *  - rtc_boot_us: RTC counter (alive across deep sleep)     → sleep time */
    uint64_t t_boot_us   = (uint64_t)esp_timer_get_time();
    uint64_t rtc_boot_us = esp_clk_rtc_time();

    if (rtc_boot_us == 0) {
        ESP_LOGW(TAG, "RTC clock unavailable (CONFIG_ESP_TIME_FUNCS_USE_RTC_TIMER "
                      "off) - slot advance falls back to the nominal interval");
    }

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

    /* Load persisted thresholds and schedule state from NVS (overrides defaults
     * if present). The schedule day counter is restored so a reset cannot repeat
     * the same day's watering. */
    load_threshold_from_nvs();
    load_schedule_state_from_nvs();

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

    /* Load committed baselines from NVS (validity gate for the boot sync). */
    baseline_init();

    app_config_t *cfg = config_get();

    /* Did RTC already hold a slot before this wake? (true on deep-sleep wake,
     * false after any reset that cleared RTC.) Used to advance the slot by the
     * sleep interval that just elapsed. */
    bool slot_rtc_valid = config_slot_valid();

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

#if BUTTON_OVERRIDES_TIMED_RUN
        ESP_LOGI(TAG, "Button wake - toggling pump (manual override)");
        pump_toggle();
        irrigation_cancel_timed_run();
#else
        /* Protect an active timed run (schedule / relay-ON duration): a button
         * press must not cut the watering short. Manual control is still
         * available through the gateway (0x02 / 0x03 / 0x07). */
        uint32_t run_left_s = irrigation_timed_run_remaining_s();
        if (run_left_s > 0) {
            ESP_LOGI(TAG, "Button wake during a timed run - pump stays ON (%lu s left)",
                     (unsigned long)run_left_s);
            cfg->interval = (uint16_t)run_left_s;   /* wake again when it ends */
        } else {
            ESP_LOGI(TAG, "Button wake - toggling pump");
            pump_toggle();
        }
#endif

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
        node_time_mark_sleep(t_boot_us);   /* record awake time + RTC mark */
        lora_sleep();          /* needed before deep sleep */
        power_deep_sleep();
        /* never reaches here */
    }

    /* ── Timer Wake or Cold Boot cycle ── */

    /* Step 4b: Baseline / time sync — runs only when needed (blocking).
     * A cold boot or any reset clears RTC, so the slot is unknown; the
     * baseline may also be absent from NVS. Must complete before the normal
     * cycle so interpolation uses a valid slot. */
    node_boot_sync();

    /* Step 4c: Advance the slot estimate by the REAL elapsed time (RTC clock),
     * not by the requested sleep interval. The RTC counter keeps running during
     * deep sleep, so this delta automatically covers everything the nominal
     * interval throws away: boot overhead (ROM/IDF), the awake part of the
     * previous cycle (LoRa windows, boot-sync listening, park near t=0) and the
     * difference between requested and actual sleep duration. */
    uint64_t rtc_now_us = esp_clk_rtc_time();

    if (!slot_rtc_valid) {
        /* Reset: the RTC domain restarted, so old marks are meaningless and the
         * boot sync has just re-synced the slot from the gateway. Restart the
         * measurement from this point. */
        ESP_LOGI(TAG, "TIME: RTC time base restarted (reset) — measurement reset");
        s_cycles_timed   = 0;
        s_drift_total_us = 0;
        s_awake_prev_us  = 0;
        s_rtc_sleep_us   = 0;
    } else if (rtc_now_us == 0) {
        /* RTC timekeeping unavailable — fall back to the nominal interval
         * (old behaviour) instead of not advancing the slot at all. */
        config_advance_slot(cfg->interval);
    } else {
        if (s_rtc_mark_us != 0 && rtc_now_us > s_rtc_mark_us) {
            uint64_t real_us = rtc_now_us - s_rtc_mark_us;
            if (real_us > NODE_TIME_MAX_DELTA_US) {
                ESP_LOGW(TAG, "TIME: RTC delta %llu ms implausible — slot NOT advanced",
                         (unsigned long long)(real_us / 1000ULL));
            } else {
                int64_t drift_us = (int64_t)real_us -
                                   (int64_t)cfg->interval * 1000000LL;
                config_advance_slot_us(real_us);
                s_cycles_timed++;
                s_drift_total_us += drift_us;
                ESP_LOGI(TAG, "TIME: cycle %llu real %llu ms | awake(prev) %llu ms | "
                         "drift %+lld ms | total %+lld ms",
                         (unsigned long long)s_cycles_timed,
                         (unsigned long long)(real_us / 1000ULL),
                         (unsigned long long)(s_awake_prev_us / 1000ULL),
                         (long long)(drift_us / 1000),
                         (long long)(s_drift_total_us / 1000));
            }
        }

        /* Sleep + boot overhead = time from the pre-sleep mark to this boot.
         * "extra" is exactly what the nominal interval does not account for. */
        if (s_rtc_sleep_us != 0 && rtc_boot_us > s_rtc_sleep_us) {
            int64_t sleep_boot_us = (int64_t)(rtc_boot_us - s_rtc_sleep_us);
            int64_t extra_us      = sleep_boot_us -
                                    (int64_t)cfg->interval * 1000000LL;
            ESP_LOGI(TAG, "TIME: sleep+boot %lld ms (requested %u ms, extra %+lld ms)",
                     (long long)(sleep_boot_us / 1000),
                     (unsigned)(cfg->interval * 1000U),
                     (long long)(extra_us / 1000));
        }
    }
    s_rtc_mark_us = rtc_now_us;

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
    node_finish_baseline_if_ready();

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

    /* Baseline gating: send when a reading deviates from the interpolated
     * baseline by more than its tolerance (temp 1, hum 2, soil 2). Only active
     * once a slot is synced AND at least one series has a stored baseline —
     * otherwise baseline_should_send() returns false for every series.
     * The returned mask bits line up with PRESENCE_TEMP/HUM/SOIL. */
    uint8_t base_mask = 0;
    bool base_send = config_slot_valid() &&
                     baseline_should_send(&sensor_data, config_get_slot(), &base_mask);

    /* DEBUG: which branch will win in the send decision below */
    ESP_LOGI(TAG, "DBG send-decision: lastSent=%d delta=0x%02X exceeded=%d "
             "base=0x%02X slot=%u hb=%d cyclesSinceSend=%d hb_cycles=%d",
             cfg->lastSentTemp, delta, exceeded, base_mask, config_get_slot(),
             data_heartbeat, cfg->cyclesSinceSend, HEARTBEAT_CYCLES);

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
    } else if (base_send) {
        /* Deviation from the interpolated baseline — send only the offending
         * series plus any delta-changed fields. base_mask bits match the
         * PRESENCE_* bits for temp/hum/soil. */
        uint8_t presence = (uint8_t)(delta | base_mask);
        ESP_LOGI(TAG, "Baseline deviation 0x%02X at slot %u — sending compact update (presence 0x%02X)",
                 base_mask, config_get_slot(), presence);
        if (commands_send_compact_with_ack(cfg->nodeId, &sensor_data, presence) == 0) {
            node_store_last_sent_compact(&sensor_data, presence);
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
    node_finish_baseline_if_ready();

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
    ESP_LOGI(TAG, "  Slot:          %u (%s, +%lu ms in slot, sync phase %+ld ms)",
             config_get_slot(), config_slot_valid() ? "valid" : "INVALID",
             (unsigned long)(config_slot_elapsed_us() / 1000UL),
             (long)(config_get_slot_error_us() / 1000));
    ESP_LOGI(TAG, "========================");

    /* Step 14b: stamp awake time + RTC mark just before sleeping. The next
     * cycle turns this into "sleep+boot" and "extra". NOTE: the park step may
     * deep-sleep for a short nap first — that nap is then attributed to the
     * next cycle's sleep+boot figure. */
    node_time_mark_sleep(t_boot_us);

    /* Step 15: Enter deep sleep (park near the t=0 boundary if close) */
    node_park_near_boundary();
    power_deep_sleep();
}
