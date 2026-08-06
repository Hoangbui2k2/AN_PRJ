#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_NAMESPACE "gateway_cfg"

#define MAX_SSID_LEN     32
#define MAX_PASS_LEN     64
#define MAX_BROKER_LEN   128
#define MAX_USERNAME_LEN 64
#define MAX_SITE_LEN     32
#define MAX_GW_ID_LEN    32

/* Number of nodes pre-configured at startup */
#define DEFAULT_NODE_COUNT 2

/**
 * @brief Default node configuration
 *
 * Values used as defaults when the server sends a command that omits
 * a parameter (e.g. set_threshold without low/high).
 */
typedef struct {
    uint8_t  id;                 /* Node ID */
    uint8_t  threshold_low;      /* Absolute threshold low (%) */
    uint8_t  threshold_high;     /* Absolute threshold high (%) */
    uint8_t  delta_temp;         /* Delta temp (×10, e.g. 20 = 2.0°C) */
    uint8_t  delta_hum;          /* Delta humidity (%) */
    uint8_t  delta_soil;         /* Delta soil moisture (%) */
    uint8_t  delta_battery;      /* Delta battery (%) */
    uint16_t report_interval;    /* Report interval (seconds) */
    uint16_t heartbeat_interval; /* Heartbeat interval (seconds) */
    uint8_t  schedule_hour;      /* Default schedule hour (0-23) */
    uint8_t  schedule_minute;    /* Default schedule minute (0-59) */
    uint8_t  mode;               /* Default operating mode */
} node_config_t;

/**
 * @brief Gateway configuration structure
 */
typedef struct {
    char site[MAX_SITE_LEN];            /**< Site identifier (e.g., "factory_1") */
    char gateway_id[MAX_GW_ID_LEN];     /**< Gateway identifier (e.g., "gw_01") */
    char wifi_ssid[MAX_SSID_LEN];
    char wifi_password[MAX_PASS_LEN];
    char mqtt_broker_uri[MAX_BROKER_LEN];
    char mqtt_username[MAX_USERNAME_LEN];
    char mqtt_password[MAX_USERNAME_LEN];
    uint32_t mqtt_port;
    node_config_t nodes[DEFAULT_NODE_COUNT]; /**< Default nodes pre-configured */
} gateway_config_t;

/**
 * @brief Initialize NVS and load saved configuration
 * 
 * @param config Pointer to config structure to populate
 * @return esp_err_t ESP_OK on success
 */
esp_err_t config_init(gateway_config_t *config);

/**
 * @brief Save WiFi credentials to NVS
 * 
 * @param ssid WiFi SSID
 * @param password WiFi password
 * @return esp_err_t ESP_OK on success
 */
esp_err_t config_save_wifi(const char *ssid, const char *password);

/**
 * @brief Save MQTT broker settings to NVS
 * 
 * @param broker_uri MQTT broker URI
 * @param username MQTT username (optional)
 * @param password MQTT password (optional)
 * @param port MQTT broker port
 * @return esp_err_t ESP_OK on success
 */
esp_err_t config_save_mqtt(const char *broker_uri, const char *username, 
                           const char *password, uint32_t port);

/**
 * @brief Save gateway identity (site + gateway_id) to NVS
 * 
 * @param site Site name (e.g., "factory_1")
 * @param gateway_id Gateway identifier (e.g., "gw_01")
 * @return esp_err_t ESP_OK on success
 */
esp_err_t config_save_identity(const char *site, const char *gateway_id);

/**
 * @brief Set default configuration values
 * 
 * @param config Pointer to config structure
 */
void config_set_defaults(gateway_config_t *config);

/**
 * @brief Print current configuration to log
 * 
 * @param config Pointer to config structure
 */
void config_print(const gateway_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */
