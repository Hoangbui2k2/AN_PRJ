#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum topic length */
#define MQTT_TOPIC_MAX    64
#define MQTT_PAYLOAD_MAX  256

/**
 * @brief Callback type for received MQTT commands
 * 
 * @param node_id Parsed node ID from topic
 * @param payload Raw JSON payload string
 * @param len Payload length
 */
typedef void (*mqtt_command_cb_t)(uint8_t node_id, const char *payload, size_t len);

/**
 * @brief Initialize MQTT client
 * 
 * Configures and starts the MQTT client with the given broker URI.
 * All topics are built dynamically using site + gateway_id.
 * 
 * @param broker_uri MQTT broker URI (e.g., "mqtt://test.mosquitto.org")
 * @param username MQTT username (can be NULL)
 * @param password MQTT password (can be NULL)
 * @param port MQTT broker port
 * @param site Site name (e.g., "factory_1")
 * @param gateway_id Gateway identifier (e.g., "gw_01")
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mqtt_app_init(const char *broker_uri, const char *username,
                        const char *password, uint32_t port,
                        const char *site, const char *gateway_id);

/**
 * @brief Set the command callback for incoming MQTT messages
 * 
 * @param cb Callback function pointer
 */
void mqtt_set_command_callback(mqtt_command_cb_t cb);

/**
 * @brief Publish data to a topic
 * 
 * @param topic Topic string
 * @param payload Payload string
 * @param qos QoS level (0, 1, or 2)
 * @return int Message ID on success, -1 on failure
 */
int mqtt_publish(const char *topic, const char *payload, int qos);

/**
 * @brief Subscribe to a topic
 * 
 * @param topic Topic string
 * @param qos QoS level
 * @return int Message ID on success, -1 on failure
 */
int mqtt_subscribe(const char *topic, int qos);

/**
 * @brief Unsubscribe from a topic
 * 
 * @param topic Topic string
 * @return int Message ID on success, -1 on failure
 */
int mqtt_unsubscribe(const char *topic);

/**
 * @brief Check if MQTT client is connected
 * 
 * @return true if connected
 * @return false if disconnected
 */
bool mqtt_is_connected(void);

/**
 * @brief Get the MQTT client handle for direct use with esp_mqtt_client API
 * 
 * @return void* MQTT client handle, or NULL if not initialized
 */
void* mqtt_get_client(void);

/**
 * @brief Stop and destroy the MQTT client
 */
void mqtt_app_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_CLIENT_H */
