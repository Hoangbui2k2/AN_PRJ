#include "sensors.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"
#include <string.h>

static const char *TAG = "SENSORS";

/* ADC oneshot handle (initialised once in sensors_init) */
static adc_oneshot_unit_handle_t s_adc_handle = NULL;

/* DHT22 timing constants (microseconds) */
#define DHT22_START_SIGNAL_US   2000  /* 2ms start signal */
#define DHT22_TIMEOUT_US        150
#define DHT22_BIT_THRESHOLD_US  35

static sensor_data_t s_last_data;

/* ──────────── DHT22 Low-Level ──────────── */

static IRAM_ATTR void dht22_set_output(void)
{
    gpio_set_direction(DHT22_GPIO, GPIO_MODE_OUTPUT_OD);
}

static IRAM_ATTR void dht22_set_input(void)
{
    gpio_set_direction(DHT22_GPIO, GPIO_MODE_INPUT);
}

static IRAM_ATTR int dht22_wait_level(int level, int timeout_us)
{
    int elapsed = 0;
    while (elapsed < timeout_us) {
        if (gpio_get_level(DHT22_GPIO) == level) {
            return 0;
        }
        ets_delay_us(1);
        elapsed++;
    }
    return -1;
}

/**
 * @brief Read 40 bits from DHT22 with retries
 * @return 0 on success, -1 on failure
 */
static int dht22_read_raw(uint8_t data[5])
{
    int retries = 3;

    for (int attempt = 0; attempt < retries; attempt++) {
        memset(data, 0, 5);

        /* Host start signal: drive low for 2ms, then release (pulled high by external pullup) */
        dht22_set_output();
        gpio_set_level(DHT22_GPIO, 0);
        ets_delay_us(DHT22_START_SIGNAL_US);
        gpio_set_level(DHT22_GPIO, 1);
        ets_delay_us(30);
        dht22_set_input();

        /* Wait for DHT22 response: DHT pulls low for ~80us, then high for ~80us */
        if (dht22_wait_level(0, DHT22_TIMEOUT_US) != 0) {
            ESP_LOGD(TAG, "DHT22: timeout waiting for response low (attempt %d/%d)", attempt + 1, retries);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (dht22_wait_level(1, DHT22_TIMEOUT_US) != 0) {
            ESP_LOGD(TAG, "DHT22: timeout waiting for response high (attempt %d/%d)", attempt + 1, retries);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Read 40 bits */
        for (int bit = 0; bit < 40; bit++) {
            /* Each bit starts with a 50us low period. Wait for it to go LOW. */
            if (dht22_wait_level(0, DHT22_TIMEOUT_US) != 0) {
                ESP_LOGD(TAG, "DHT22: timeout bit %d low", bit);
                goto try_next_attempt;
            }

            /* Wait for the line to go HIGH to start measuring the high pulse. */
            if (dht22_wait_level(1, DHT22_TIMEOUT_US) != 0) {
                ESP_LOGD(TAG, "DHT22: timeout bit %d high wait", bit);
                goto try_next_attempt;
            }

            /* Measure high pulse duration: ~26-28us = '0', ~70us = '1' */
            uint32_t high_us = 0;
            while (gpio_get_level(DHT22_GPIO) == 1) {
                high_us++;
                ets_delay_us(1);
                if (high_us > DHT22_TIMEOUT_US) {
                    ESP_LOGD(TAG, "DHT22: timeout bit %d measuring high pulse", bit);
                    goto try_next_attempt;
                }
            }

            int byte_idx = bit / 8;
            int bit_pos  = 7 - (bit % 8);
            if (high_us > DHT22_BIT_THRESHOLD_US) {
                data[byte_idx] |= (1 << bit_pos);
            }
        }

        /* Verify checksum: sum of first 4 bytes, masked to 8 bits, should equal byte 4 */
        uint8_t sum = (data[0] + data[1] + data[2] + data[3]) & 0xFF;
        if (sum == data[4]) {
            return 0;
        }
        ESP_LOGD(TAG, "DHT22: checksum 0x%02X != expected 0x%02X (attempt %d)",
                 sum, data[4], attempt + 1);

try_next_attempt:
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    return -1;
}

/* ──────────── Soil Moisture ADC ──────────── */

static int read_soil_moisture_raw(void)
{
    int raw = 0;
    int samples = 16;
    int valid = 0;

    for (int i = 0; i < samples; i++) {
        int val = 0;
        if (adc_oneshot_read(s_adc_handle, SOIL_MOISTURE_ADC_CH, &val) == ESP_OK) {
            if (val >= 0 && val <= 4095) {
                raw += val;
                valid++;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(3));
    }

    if (valid == 0) {
        return -1;   /* All reads failed */
    }
    return raw / valid;
}

#define SOIL_RAW_DRY     4095   // DRY Đo khi cảm biến khô ráo (hoặc giá trị max bạn đo được)
#define SOIL_RAW_WET     2600   // Giá trị trung bình bạn đo khi nhúng trong nước

static uint8_t map_soil_raw_to_pct(int raw)
{
    if (raw > SOIL_RAW_DRY) raw = SOIL_RAW_DRY;
    if (raw < SOIL_RAW_WET) raw = SOIL_RAW_WET;

    return (uint8_t)((SOIL_RAW_DRY - raw) * 100 / (SOIL_RAW_DRY - SOIL_RAW_WET));
}
/* ──────────── Battery (via GPIO36 / ADC1_CH0) ──────────── */

/**
 * @brief Read battery voltage through external 1:1 voltage divider on GPIO36.
 *
 *        Vbat = ADC_raw * 3.3V / 4095 * DIVIDER_RATIO(2)
 *
 * @return Battery level 0–100 %, or 0xFF if read failed.
 */
static uint8_t read_battery_level(void)
{
    int raw = 0;
    esp_err_t ret = adc_oneshot_read(s_adc_handle, BATTERY_ADC_CH, &raw);
    if (ret != ESP_OK || raw < 0 || raw > 4095) {
        ESP_LOGE(TAG, "Battery ADC read failed (ret=%d, raw=%d)", ret, raw);
        return 0xFF;   /* Unknown / external power */
    }

    /* Vdiv = raw * 3.3 / 4095 ;  Vbat = Vdiv * 2  (divider 100k+100k) */
    float vbat = (float)raw * 3.3f * BATTERY_DIVIDER_RATIO / 4095.0f;

    /* Map 18650 voltage to percentage (3.3V–4.2V) */
    if (vbat >= BATTERY_VOLT_FULL) return 100;
    if (vbat <= BATTERY_VOLT_EMPTY) return 0;

    uint8_t pct = (uint8_t)((vbat - BATTERY_VOLT_EMPTY) /
                            (BATTERY_VOLT_FULL - BATTERY_VOLT_EMPTY) * 100.0f);

    ESP_LOGI(TAG, "Battery: raw=%d, Vbat=%.2fV, %d%%", raw, vbat, pct);
    return pct;
}

/* ──────────── Public API ──────────── */

void sensors_init(void)
{
    ESP_LOGI(TAG, "Initializing sensors");

    memset(&s_last_data, 0, sizeof(sensor_data_t));

    /* Configure DHT22 GPIO with internal pull-up and output high */
    gpio_config_t dht_cfg = {
        .pin_bit_mask = (1ULL << DHT22_GPIO),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&dht_cfg);
    gpio_set_level(DHT22_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    /* ── Initialise ADC1 oneshot driver for soil moisture ── */
    adc_oneshot_unit_init_cfg_t adc_init = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&adc_init, &s_adc_handle));

    adc_oneshot_chan_cfg_t adc_chan = {
        .atten = ADC_ATTEN_DB_12,       /* 0 – ~3.3 V input range */
        .bitwidth = ADC_BITWIDTH_12,    /* 12-bit resolution (0-4095) */
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, SOIL_MOISTURE_ADC_CH, &adc_chan));

    /* ── Configure same ADC1 channel for battery (GPIO36 / ADC1_CH0) ── */
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, BATTERY_ADC_CH, &adc_chan));

    ESP_LOGI(TAG, "Sensors initialized: DHT22=GPIO%d, Soil=ADC1_CH4(GPIO32), Battery=ADC1_CH0(GPIO36)",
             DHT22_GPIO);
}

void sensors_read(sensor_data_t *data)
{
    if (data == NULL) return;

    memset(data, 0, sizeof(sensor_data_t));
    data->sensor_error = 0;

    /* ── Read battery voltage via GPIO36 / ADC1_CH0 ── */
    data->battery = read_battery_level();

    /* ── Read DHT22 ── */
    uint8_t dht_raw[5] = {0};
    if (dht22_read_raw(dht_raw) == 0) {
        uint16_t raw_hum  = ((uint16_t)dht_raw[0] << 8) | dht_raw[1];
        uint16_t raw_temp = ((uint16_t)dht_raw[2] << 8) | dht_raw[3];

        if (raw_temp & 0x8000) {
            raw_temp &= 0x7FFF;
            data->temperature = -(float)raw_temp / 10.0f;
        } else {
            data->temperature = (float)raw_temp / 10.0f;
        }
        data->humidity = (float)raw_hum / 10.0f;

        data->temperature_int = (int)(data->temperature * 10);
        data->humidity_int    = (int)(data->humidity * 10);

        ESP_LOGI(TAG, "DHT22 read success: T=%.1f C, H=%.1f %%", data->temperature, data->humidity);
    } else {
        data->sensor_error |= 0x01;
        data->temperature = 0.0f;
        data->humidity = 0.0f;
        data->temperature_int = 0;
        data->humidity_int = 0;
        ESP_LOGE(TAG, "DHT22 read failed");
    }

    /* ── Read soil moisture ── */
    int soil_raw = read_soil_moisture_raw();
    if (soil_raw >= 0 && soil_raw <= 4095) {
        data->soil_moisture_raw = (uint16_t)soil_raw;
        data->soil_moisture_pct = map_soil_raw_to_pct(soil_raw);
        ESP_LOGI(TAG, "Soil moisture: raw=%d, pct=%d%%", soil_raw, data->soil_moisture_pct);
    } else {
        data->soil_moisture_raw = 4095; /* Default to dry */
        data->soil_moisture_pct = 0;
        data->sensor_error |= 0x02;
        ESP_LOGE(TAG, "Soil moisture read failed (raw=%d)", soil_raw);
    }

    /* Store as last known data */
    memcpy(&s_last_data, data, sizeof(sensor_data_t));
}

bool sensors_get_last(sensor_data_t *data)
{
    if (data == NULL) return false;
    memcpy(data, &s_last_data, sizeof(sensor_data_t));
    return true;
}

bool sensors_dht_error(void)
{
    return (s_last_data.sensor_error & 0x01) != 0;
}

bool sensors_soil_error(void)
{
    return (s_last_data.sensor_error & 0x02) != 0;
}
