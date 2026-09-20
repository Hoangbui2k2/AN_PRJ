#include "power.h"
#include "config.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "POWER";

/* Debounce state for button */
static uint32_t s_last_button_change_tick = 0;
static bool s_last_button_level = 1;

void power_init(void)
{
    ESP_LOGI(TAG, "Initializing power management");

    /* Configure sensor power MOSFET output - ensure HIGH (OFF) BEFORE enabling output driver */
    gpio_set_level(SENSOR_POWER_GPIO, 1);  /* HIGH = OFF before gpio_config */

    gpio_config_t power_pin = {
        .pin_bit_mask = (1ULL << SENSOR_POWER_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&power_pin);

    /* Configure wake button (GPIO34 is input-only, no internal pull-ups supported) */
    gpio_config_t button_pin = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&button_pin);

    /* Ensure sensor power starts OFF (MOSFET gate HIGH) */
    power_sensor_off();

    /* Record initial button state for debounce */
    s_last_button_level = gpio_get_level(BUTTON_GPIO);
    s_last_button_change_tick = xTaskGetTickCount();

    ESP_LOGI(TAG, "Power initialized: sensor MOSFET=GPIO%d (Active-Low), button=GPIO%d (External Pull-Up)",
             SENSOR_POWER_GPIO, BUTTON_GPIO);
}

void power_sensor_on(void)
{
    /* LOW = ON */
    gpio_set_level(SENSOR_POWER_GPIO, 0);
    ESP_LOGD(TAG, "Sensor power ON (GPIO%d LOW)", SENSOR_POWER_GPIO);
}

void power_sensor_off(void)
{
    /* HIGH = OFF */
    gpio_set_level(SENSOR_POWER_GPIO, 1);
    ESP_LOGD(TAG, "Sensor power OFF (GPIO%d HIGH)", SENSOR_POWER_GPIO);
}

bool power_button_pressed(void)
{
    bool current = gpio_get_level(BUTTON_GPIO);
    uint32_t now = xTaskGetTickCount();

    /* Detect state change with debounce (50ms) */
    if (current != s_last_button_level &&
        (now - s_last_button_change_tick) >= pdMS_TO_TICKS(50)) {
        s_last_button_level = current;
        s_last_button_change_tick = now;
        /* Active-low button: pressed when LOW */
        return (current == 0);
    }

    return false;
}

void power_set_sleep_timer(uint32_t seconds)
{
    if (seconds < MIN_SLEEP_SEC) seconds = MIN_SLEEP_SEC;
    if (seconds > MAX_SLEEP_SEC) seconds = MAX_SLEEP_SEC;
    app_config_t *cfg = config_get();
    cfg->interval = seconds;
    cfg->normalInterval = seconds;   /* keep the timed-run restore value in sync */
    config_save();
    ESP_LOGI(TAG, "Sleep timer set to %lu s", (unsigned long)seconds);
}

esp_sleep_wakeup_cause_t power_get_wake_cause(void)
{
    /* Note: EXT0 wakeup on ESP32 is reported as ESP_SLEEP_WAKEUP_EXT0 */
    return esp_sleep_get_wakeup_cause();
}

void power_deep_sleep(void)
{
    app_config_t *cfg = config_get();
    power_deep_sleep_for(cfg->interval);
}

/**
 * @brief Deep sleep for a custom number of seconds (does NOT change the
 *        configured interval). Used to park awake shortly before the t=0
 *        boundary without disturbing the configured cycle.
 */
void power_deep_sleep_for(uint32_t sleep_sec)
{
    if (sleep_sec < MIN_SLEEP_SEC) sleep_sec = MIN_SLEEP_SEC;
    if (sleep_sec > MAX_SLEEP_SEC) sleep_sec = MAX_SLEEP_SEC;

    ESP_LOGI(TAG, "Entering deep sleep for %lu s", (unsigned long)sleep_sec);

    /* ── Configure power domains ──
     *
     * We MUST keep RTC_PERIPH powered during sleep because the EXT0 wakeup
     * (button GPIO34) uses the RTC IO controller which lives in this domain.
     * RTC_SLOW_MEM must also stay on so that RTC_DATA_ATTR configuration
     * survives the sleep cycle.
     *
     * These calls override any Kconfig defaults that might otherwise power
     * these domains down for extra saving. */
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH,   ESP_PD_OPTION_ON);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM,  ESP_PD_OPTION_ON);

    /* ── Configure wake sources ── */
    esp_sleep_enable_timer_wakeup((uint64_t)sleep_sec * 1000000ULL);

    /* GPIO wake on button (active-low, ext0 wakeup is suited for single RTC IO wakeup) */
    esp_sleep_enable_ext0_wakeup(BUTTON_GPIO, 0);

    /* ── Hold RTC-capable output GPIOs so they retain their level during sleep.
     *
     * SENSOR_POWER_GPIO (GPIO35 / RTC_GPIO5) must stay HIGH (MOSFET OFF) to
     * keep sensors unpowered and avoid wasting current through the DHT22
     * pull-up and the soil-moisture ADC divider.
     *
     * Non-RTC GPIOs (LoRa MD0/MD1 on GPIO22/21, pump toggle on GPIO23, etc.)
     * float during deep sleep; external pull resistors on the PCB are required
     * to keep them in a known state. */
    rtc_gpio_hold_en(SENSOR_POWER_GPIO);

    /* Log before sleeping */
    ESP_LOGI(TAG, "Deep sleep starting (wake: timer=%lus, button=GPIO%d)",
             (unsigned long)sleep_sec, BUTTON_GPIO);
    ESP_LOGI(TAG, "Free heap before sleep: %lu bytes",
             (unsigned long)esp_get_free_heap_size());

    /* Enter deep sleep - never returns */
    esp_deep_sleep_start();
}
