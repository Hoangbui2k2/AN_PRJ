#include "pump.h"
#include "config.h"
#include "power.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "PUMP";

static uint8_t s_pump_state = 0;

void pump_init(void)
{
    ESP_LOGI(TAG, "Initializing pump control on GPIO%d", PUMP_TOGGLE_GPIO);

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PUMP_TOGGLE_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    /* Ensure relay control starts LOW at rest */
    gpio_set_level(PUMP_TOGGLE_GPIO, 0);

    /* Sync state to the CD4013 toggle flip-flop.
     *
     * Hardware: D1=Q̅1 (toggle), SET1=RESET1=GND, Q1 -> AO3400A MOSFET
     * (active-high) -> pump.  Q1 toggles on each rising edge of CLOCK1 and is
     * HELD across deep sleep, so the tracked state survives wake cycles.
     *
     * The CD4013 power-on state of Q1 is indeterminate (part-dependent); on
     * this unit it powers up LOW -> pump starts OFF at power-on, matching the
     * RTC default.  PUMP_COLDBOOT_STATE is written back to RTC on cold boot so
     * cfg->pumpState never diverges from s_pump_state (irrigation checks
     * cfg->pumpState while pump_on/off check s_pump_state).  Without this
     * sync, a stale value would be restored on the next wake and the pump
     * could get toggled to the wrong state.
     *   - Deep-sleep wake: flip-flop holds its output, so restore RTC. */
    app_config_t *cfg_rtc = config_get();
    esp_sleep_wakeup_cause_t wake = power_get_wake_cause();
    if (wake == ESP_SLEEP_WAKEUP_UNDEFINED) {
        s_pump_state = PUMP_COLDBOOT_STATE;
        cfg_rtc->pumpState = (PUMP_COLDBOOT_STATE == 1);   /* keep RTC in sync */
    } else {
        s_pump_state = cfg_rtc->pumpState ? 1 : 0;
    }

    ESP_LOGI(TAG, "Pump initialized, state: %s (wake: %d)",
             s_pump_state ? "ON" : "OFF", wake);
}

/**
 * @brief Send a 20ms HIGH pulse to the CD4013 CLOCK1 input.
 *
 * The CD4013 toggles Q1 on the RISING edge of CLOCK1, so one HIGH->LOW pulse
 * produces exactly one toggle (the trailing falling edge does nothing).
 */
static bool send_toggle_pulse(void)
{
    /* Raise pulse */
    gpio_set_level(PUMP_TOGGLE_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(PUMP_PULSE_MS));

    /* Lower pulse */
    gpio_set_level(PUMP_TOGGLE_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(PUMP_DEBOUNCE_MS));

    /* Read back to verify it returned to LOW */
    if (gpio_get_level(PUMP_TOGGLE_GPIO) != 0) {
        ESP_LOGE(TAG, "GPIO stuck HIGH after pulse");
        return false;
    }

    return true;
}

bool pump_toggle(void)
{
    ESP_LOGI(TAG, "Toggling pump (current state: %s)", s_pump_state ? "ON" : "OFF");

    if (!send_toggle_pulse()) {
        ESP_LOGE(TAG, "Toggle pulse failed");
        config_set_alarm(ALARM_RELAY_ERROR);
        return false;
    }

    /* Update state (CD4013 flips on each clock pulse) */
    s_pump_state = s_pump_state ? 0 : 1;
    config_set_pump_state(s_pump_state == 1);

    ESP_LOGI(TAG, "Pump toggled to %s", s_pump_state ? "ON" : "OFF");
    return true;
}

bool pump_on(void)
{
    if (s_pump_state == 0) {
        return pump_toggle();
    }
    ESP_LOGD(TAG, "Pump already ON");
    return true;
}

bool pump_off(void)
{
    if (s_pump_state == 1) {
        return pump_toggle();
    }
    ESP_LOGD(TAG, "Pump already OFF");
    return true;
}

uint8_t pump_get_state(void)
{
    return s_pump_state;
}

bool pump_verify_state(void)
{
    /* Verify no electrical fault (GPIO should be LOW at rest) */
    if (gpio_get_level(PUMP_TOGGLE_GPIO) != 0) {
        ESP_LOGE(TAG, "Pump GPIO stuck HIGH - relay driver fault");
        config_set_alarm(ALARM_RELAY_ERROR);
        return false;
    }
    return true;
}
