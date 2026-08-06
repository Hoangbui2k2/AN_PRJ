#include "alarms.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ALARMS";

void alarms_init(void)
{
    ESP_LOGI(TAG, "Initializing alarm LED on GPIO%d", ALARM_LED_GPIO);

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << ALARM_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    gpio_set_level(ALARM_LED_GPIO, 0);
}

int alarms_get_blink_count(alarm_type_t alarm)
{
    switch (alarm) {
        case ALARM_SENSOR_ERROR:
            return BLINKS_SENSOR_ERROR;
        case ALARM_SOIL_OUT_RANGE:
            return BLINKS_SOIL_OUT_RANGE;
        case ALARM_RELAY_ERROR:
            return BLINKS_RELAY_ERROR;
        case ALARM_LOW_BATTERY:
            return BLINKS_LOW_BATTERY;
        case ALARM_GATEWAY_LOST:
            return BLINKS_GATEWAY_LOST;
        default:
            return 0;
    }
}

void alarms_signal(alarm_type_t alarm)
{
    int blinks = alarms_get_blink_count(alarm);
    if (blinks == 0) {
        ESP_LOGW(TAG, "No blink pattern for alarm 0x%02X", alarm);
        return;
    }

    ESP_LOGI(TAG, "Signalling alarm 0x%02X (%d blinks, %d repeats)",
             alarm, blinks, ALARM_REPEAT_COUNT);

    for (int repeat = 0; repeat < ALARM_REPEAT_COUNT; repeat++) {
        for (int i = 0; i < blinks; i++) {
            gpio_set_level(ALARM_LED_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(ALARM_BLINK_MS));
            gpio_set_level(ALARM_LED_GPIO, 0);

            if (i < blinks - 1) {
                vTaskDelay(pdMS_TO_TICKS(ALARM_BLINK_GAP_MS));
            }
        }

        if (repeat < ALARM_REPEAT_COUNT - 1) {
            vTaskDelay(pdMS_TO_TICKS(ALARM_PATTERN_GAP_MS));
        }
    }
}

void alarm_led_on(void)
{
    gpio_set_level(ALARM_LED_GPIO, 1);
}

void alarm_led_off(void)
{
    gpio_set_level(ALARM_LED_GPIO, 0);
}
