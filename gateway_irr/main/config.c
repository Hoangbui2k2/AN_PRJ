#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "config.h"
#include "node_manager.h"

static const char *TAG = "CONFIG";

static const char *KEY_SITE       = "site";
static const char *KEY_GW_ID      = "gw_id";
static const char *KEY_WIFI_SSID  = "wifi_ssid";
static const char *KEY_WIFI_PASS  = "wifi_pass";
static const char *KEY_MQTT_URI   = "mqtt_uri";
static const char *KEY_MQTT_USER  = "mqtt_user";
static const char *KEY_MQTT_PASS  = "mqtt_pass";
static const char *KEY_MQTT_PORT  = "mqtt_port";
static const char *KEY_MQTT_TYPE  = "mqtt_type";
static const char *KEY_MQTT_CLIENT_ID = "mqtt_client_id";

void config_set_defaults(gateway_config_t *config)
{
    memset(config, 0, sizeof(gateway_config_t));
    strlcpy(config->site, "HCM", sizeof(config->site));
    strlcpy(config->gateway_id, "gw_01", sizeof(config->gateway_id));
    // strlcpy(config->wifi_ssid, "Hoa Hau", sizeof(config->wifi_ssid));
    // strlcpy(config->wifi_password, "12233445", sizeof(config->wifi_password));
        strlcpy(config->wifi_ssid, "Hoa Hau", sizeof(config->wifi_ssid));
    strlcpy(config->wifi_password, "12233445", sizeof(config->wifi_password));
    strlcpy(config->mqtt_broker_uri, "mqtts://a1xel4n1u7sh7s-ats.iot.ap-southeast-1.amazonaws.com", sizeof(config->mqtt_broker_uri));
    config->mqtt_port = 8883;
    strlcpy(config->mqtt_username, "hivemq.webclient.1742180699133", sizeof(config->mqtt_username));
    strlcpy(config->mqtt_password, "#x1V7:H62pCZ%e&nGkgR", sizeof(config->mqtt_password));
    config->mqtt_broker_type = MQTT_BROKER_AWS;
    strlcpy(config->mqtt_client_id, "gw-01", sizeof(config->mqtt_client_id));

    /* ── Default nodes (2) ──
     * These are registered at startup and their values are used as
     * defaults whenever the server sends a command without parameters. */
    config->nodes[0] = (node_config_t){
        .id = 1,
        .threshold_low = THRESHOLD_LOW_DEFAULT,
        .threshold_high = THRESHOLD_HIGH_DEFAULT,
        .delta_temp = DELTA_TEMP_DEFAULT,
        .delta_hum = DELTA_HUM_DEFAULT,
        .delta_soil = DELTA_SOIL_DEFAULT,
        .delta_battery = DELTA_BATTERY_DEFAULT,
        .report_interval = REPORT_INTERVAL_DEFAULT,
        .heartbeat_interval = 60,
        .schedule_hour = 6,
        .schedule_minute = 0,
        .mode = 1,
    };
    config->nodes[1] = (node_config_t){
        .id = 2,
        .threshold_low = THRESHOLD_LOW_DEFAULT,
        .threshold_high = THRESHOLD_HIGH_DEFAULT,
        .delta_temp = DELTA_TEMP_DEFAULT,
        .delta_hum = DELTA_HUM_DEFAULT,
        .delta_soil = DELTA_SOIL_DEFAULT,
        .delta_battery = DELTA_BATTERY_DEFAULT,
        .report_interval = REPORT_INTERVAL_DEFAULT,
        .heartbeat_interval = 60,
        .schedule_hour = 6,
        .schedule_minute = 0,
        .mode = 1,
    };
}

esp_err_t config_init(gateway_config_t *config)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");

    config_set_defaults(config);

    nvs_handle_t handle;
    ret = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No saved config found, using defaults");
        return ESP_OK;
    }

    size_t len = sizeof(config->site);
    nvs_get_str(handle, KEY_SITE, config->site, &len);

    len = sizeof(config->gateway_id);
    nvs_get_str(handle, KEY_GW_ID, config->gateway_id, &len);

    len = sizeof(config->wifi_ssid);
    nvs_get_str(handle, KEY_WIFI_SSID, config->wifi_ssid, &len);

    len = sizeof(config->wifi_password);
    nvs_get_str(handle, KEY_WIFI_PASS, config->wifi_password, &len);

    len = sizeof(config->mqtt_broker_uri);
    nvs_get_str(handle, KEY_MQTT_URI, config->mqtt_broker_uri, &len);

    len = sizeof(config->mqtt_username);
    nvs_get_str(handle, KEY_MQTT_USER, config->mqtt_username, &len);

    len = sizeof(config->mqtt_password);
    nvs_get_str(handle, KEY_MQTT_PASS, config->mqtt_password, &len);

    uint32_t port = 0;
    if (nvs_get_u32(handle, KEY_MQTT_PORT, &port) == ESP_OK) {
        config->mqtt_port = port;
    }

    uint32_t broker_type = MQTT_BROKER_HIVEMQ;
    if (nvs_get_u32(handle, KEY_MQTT_TYPE, &broker_type) == ESP_OK) {
        if (broker_type <= MQTT_BROKER_AWS) {
            config->mqtt_broker_type = (uint8_t)broker_type;
        }
    }

    len = sizeof(config->mqtt_client_id);
    nvs_get_str(handle, KEY_MQTT_CLIENT_ID, config->mqtt_client_id, &len);

    nvs_close(handle);
    ESP_LOGI(TAG, "Configuration loaded from NVS");
    return ESP_OK;
}

esp_err_t config_save_wifi(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;

    ret = nvs_set_str(handle, KEY_WIFI_SSID, ssid);
    if (ret != ESP_OK) goto cleanup;

    ret = nvs_set_str(handle, KEY_WIFI_PASS, password);
    if (ret != ESP_OK) goto cleanup;

    ret = nvs_commit(handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "WiFi config saved to NVS");
    }

cleanup:
    nvs_close(handle);
    return ret;
}

esp_err_t config_save_mqtt(const char *broker_uri, const char *username,
                           const char *password, uint32_t port)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;

    ret = nvs_set_str(handle, KEY_MQTT_URI, broker_uri);
    if (ret != ESP_OK) goto cleanup;

    if (username && strlen(username) > 0) {
        ret = nvs_set_str(handle, KEY_MQTT_USER, username);
        if (ret != ESP_OK) goto cleanup;
    }

    if (password && strlen(password) > 0) {
        ret = nvs_set_str(handle, KEY_MQTT_PASS, password);
        if (ret != ESP_OK) goto cleanup;
    }

    ret = nvs_set_u32(handle, KEY_MQTT_PORT, port);
    if (ret != ESP_OK) goto cleanup;

    ret = nvs_commit(handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "MQTT config saved to NVS");
    }

cleanup:
    nvs_close(handle);
    return ret;
}

esp_err_t config_save_identity(const char *site, const char *gateway_id)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;

    ret = nvs_set_str(handle, KEY_SITE, site);
    if (ret != ESP_OK) goto cleanup;

    ret = nvs_set_str(handle, KEY_GW_ID, gateway_id);
    if (ret != ESP_OK) goto cleanup;

    ret = nvs_commit(handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Gateway identity saved to NVS: %s / %s", site, gateway_id);
    }

cleanup:
    nvs_close(handle);
    return ret;
}

esp_err_t config_save_broker_type(uint8_t broker_type)
{
    if (broker_type > MQTT_BROKER_AWS) {
        ESP_LOGE(TAG, "Invalid broker type %d", broker_type);
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;

    ret = nvs_set_u32(handle, KEY_MQTT_TYPE, broker_type);
    if (ret != ESP_OK) goto cleanup;

    ret = nvs_commit(handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "MQTT broker type saved to NVS: %d", broker_type);
    }

cleanup:
    nvs_close(handle);
    return ret;
}

void config_print(const gateway_config_t *config)
{
    ESP_LOGI(TAG, "===== Gateway Configuration =====");
    ESP_LOGI(TAG, "Site: %s", config->site);
    ESP_LOGI(TAG, "Gateway ID: %s", config->gateway_id);
    ESP_LOGI(TAG, "WiFi SSID: %s", config->wifi_ssid);
    ESP_LOGI(TAG, "MQTT Broker: %s:%lu", config->mqtt_broker_uri, (unsigned long)config->mqtt_port);
    ESP_LOGI(TAG, "MQTT Broker Type: %d (%s)", config->mqtt_broker_type,
             config->mqtt_broker_type == MQTT_BROKER_AWS ? "AWS IoT Core (cert)"
                                                         : "HiveMQ (user/pass)");
    ESP_LOGI(TAG, "MQTT Client ID: %s", config->mqtt_client_id);
    if (strlen(config->mqtt_username) > 0) {
        ESP_LOGI(TAG, "MQTT User: %s", config->mqtt_username);
    }
    for (int i = 0; i < DEFAULT_NODE_COUNT; i++) {
        ESP_LOGI(TAG, "Default Node %d: ID=0x%02X Thr=[%d-%d] Delta=[t%d/h%d/s%d/b%d] "
                 "Report=%d s Heartbeat=%d s Schedule=%02d:%02d Mode=%d",
                 i + 1, config->nodes[i].id,
                 config->nodes[i].threshold_low, config->nodes[i].threshold_high,
                 config->nodes[i].delta_temp, config->nodes[i].delta_hum,
                 config->nodes[i].delta_soil, config->nodes[i].delta_battery,
                 config->nodes[i].report_interval, config->nodes[i].heartbeat_interval,
                 config->nodes[i].schedule_hour, config->nodes[i].schedule_minute,
                 config->nodes[i].mode);
    }
    ESP_LOGI(TAG, "================================");
}
