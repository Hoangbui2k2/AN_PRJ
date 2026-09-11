#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_event.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "mqtt.h"
#include "topic.h"
#include "config.h"
#include "certs.h"
#include "cJSON.h"

static const char *TAG = "MQTT_CLI";

/* MQTT client handle */
static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool s_connected = false;

/* Gateway identity (set during init, used to build topics) */
static char s_site[TOPIC_SITE_LEN] = "default";
static char s_gateway_id[TOPIC_GW_ID_LEN] = "gw_01";

/* Command callback */
static mqtt_command_cb_t s_command_cb = NULL;

/* Forward declaration */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data);

esp_err_t mqtt_app_init(const char *broker_uri, const char *username,
                        const char *password, uint32_t port,
                        const char *site, const char *gateway_id,
                        uint8_t broker_type, const char *client_id)
{
    /* If already initialized, clean up first */
    if (s_mqtt_client) {
        mqtt_app_stop();
    }

    ESP_LOGI(TAG, "Initializing MQTT client: %s:%lu", broker_uri, (unsigned long)port);

    /* Store gateway identity */
    if (site) {
        strlcpy(s_site, site, sizeof(s_site));
    }
    if (gateway_id) {
        strlcpy(s_gateway_id, gateway_id, sizeof(s_gateway_id));
    }
    ESP_LOGI(TAG, "Gateway identity: %s / %s", s_site, s_gateway_id);

    /* Build URI with port if not default */
    char uri[192];
    if (port != 1883 && port != 8883) {
        /* Check if URI already has port */
        if (strchr(broker_uri, ':') == strrchr(broker_uri, ':')) {
            snprintf(uri, sizeof(uri), "%s:%lu", broker_uri, (unsigned long)port);
        } else {
            snprintf(uri, sizeof(uri), "%s", broker_uri);
        }
    } else {
        snprintf(uri, sizeof(uri), "%s", broker_uri);
    }

    /* Initialize config to zero then assign fields individually */
    esp_mqtt_client_config_t mqtt_cfg = { 0 };
    mqtt_cfg.broker.address.uri = uri;

    /* Build last will topic using topic module (shared by both broker modes) */
    {
        char will_topic[TOPIC_MAX_LEN];
        topic_build(will_topic, sizeof(will_topic),
                    s_site, s_gateway_id, TOPIC_GW_WILL, 0);
        mqtt_cfg.session.last_will.topic = will_topic;
        mqtt_cfg.session.last_will.msg = "{\"status\":\"offline\"}";
        mqtt_cfg.session.last_will.qos = 1;
        mqtt_cfg.session.last_will.msg_len = 0;
    }

    if (broker_type == MQTT_BROKER_AWS) {
        /* ── AWS IoT Core: mutual-TLS with X.509 certificate ──
         * AWS does NOT support username/password. The device authenticates by
         * presenting its client certificate + private key; the server is
         * verified against the Amazon Root CA. All three PEMs are loaded from
         * the dedicated `certs` NVS partition. */
        /* Client ID must equal the IoT Thing name for policy matching. */
        mqtt_cfg.credentials.client_id = (client_id && strlen(client_id) > 0)
                                             ? client_id : s_gateway_id;

        const char *ca = certs_get_ca();
        const char *client_cert = certs_get_client_cert();
        const char *client_key = certs_get_client_key();

        if (!ca || !client_cert || !client_key) {
            ESP_LOGE(TAG, "AWS mode requires CA + client cert + key in certs partition.");
            ESP_LOGE(TAG, "Flash them using tools/gen_certs_nvs.py");
            return ESP_ERR_INVALID_STATE;
        }
        mqtt_cfg.broker.verification.certificate = ca;
        mqtt_cfg.credentials.authentication.certificate = client_cert;
        mqtt_cfg.credentials.authentication.key = client_key;

        ESP_LOGI(TAG, "Using AWS IoT Core mutual-TLS (client_id=%s)",
                 mqtt_cfg.credentials.client_id);
    } else {
        /* ── HiveMQ / generic broker: username + password over TLS ──
         * Use the ESP x509 certificate bundle (Mozilla CAs) for server
         * verification, since these brokers present a public-CA cert. */
        mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;

        if (username && strlen(username) > 0) {
            mqtt_cfg.credentials.username = username;
        }
        if (password && strlen(password) > 0) {
            mqtt_cfg.credentials.authentication.password = password;
        }
        ESP_LOGI(TAG, "Using MQTT broker with username/password auth");
    }

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_mqtt_client) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return ESP_FAIL;
    }

    /* Register event handler */
    esp_err_t ret = esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID,
                                                     mqtt_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register MQTT event handler: %s", esp_err_to_name(ret));
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
        return ret;
    }

    /* Start the client */
    ret = esp_mqtt_client_start(s_mqtt_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(ret));
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "MQTT client started");
    return ESP_OK;
}

void mqtt_set_command_callback(mqtt_command_cb_t cb)
{
    s_command_cb = cb;
}

int mqtt_publish(const char *topic, const char *payload, int qos)
{
    if (!s_mqtt_client || !s_connected) {
        ESP_LOGW(TAG, "Cannot publish: MQTT not connected");
        return -1;
    }

    int msg_id = esp_mqtt_client_publish(s_mqtt_client, topic, payload, 0, qos, 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish to %s", topic);
    } else {
        ESP_LOGD(TAG, "Published to %s (msg_id=%d)", topic, msg_id);
    }
    return msg_id;
}

int mqtt_subscribe(const char *topic, int qos)
{
    if (!s_mqtt_client) {
        ESP_LOGW(TAG, "Cannot subscribe: MQTT not initialized");
        return -1;
    }

    int msg_id = esp_mqtt_client_subscribe(s_mqtt_client, topic, qos);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to subscribe to %s", topic);
    } else {
        ESP_LOGI(TAG, "Subscribed to %s (msg_id=%d)", topic, msg_id);
    }
    return msg_id;
}

int mqtt_unsubscribe(const char *topic)
{
    if (!s_mqtt_client) {
        return -1;
    }
    return esp_mqtt_client_unsubscribe(s_mqtt_client, topic);
}

bool mqtt_is_connected(void)
{
    return s_connected;
}

void* mqtt_get_client(void)
{
    return (void*)s_mqtt_client;
}

void mqtt_app_stop(void)
{
    if (s_mqtt_client) {
        esp_mqtt_client_stop(s_mqtt_client);
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
        s_connected = false;
        ESP_LOGI(TAG, "MQTT client stopped");
    }
}

/**
 * @brief Parse node ID from MQTT topic using topic module
 * 
 * Uses topic_parse() to extract node ID from the topic.
 * 
 * @param topic Full topic string
 * @return uint8_t Node ID, or 0 if parsing fails
 */
static uint8_t parse_node_id_from_topic(const char *topic)
{
    topic_info_t info;
    if (!topic_parse(topic, &info)) {
        return 0;
    }
    /* Only return node ID for node-level topics */
    switch (info.type) {
        case TOPIC_NODE_DATA:
        case TOPIC_NODE_ALARM:
        case TOPIC_NODE_STATUS:
        case TOPIC_NODE_CMD:
            return info.node_id;
        default:
            return 0;
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    (void)event_id;

    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    if (event == NULL) {
        ESP_LOGE(TAG, "mqtt_event_handler: event_data is NULL");
        return;
    }

    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected to broker");
            s_connected = true;

            /* Subscribe to all node command topics */
            {
                char topic[TOPIC_MAX_LEN];
                topic_get_node_cmd_filter(topic, sizeof(topic),
                                          s_site, s_gateway_id);
                esp_mqtt_client_subscribe(s_mqtt_client, topic, 1);
                ESP_LOGI(TAG, "Subscribed to %s", topic);
            }

            /* Subscribe to gateway config topic */
            {
                char topic[TOPIC_MAX_LEN];
                topic_build(topic, sizeof(topic),
                            s_site, s_gateway_id, TOPIC_GW_CONFIG, 0);
                esp_mqtt_client_subscribe(s_mqtt_client, topic, 1);
                ESP_LOGI(TAG, "Subscribed to %s", topic);
            }

            /* Publish gateway online status */
            {
                char topic[TOPIC_MAX_LEN];
                topic_build(topic, sizeof(topic),
                            s_site, s_gateway_id, TOPIC_GW_STATUS, 0);
                const char *status = "{\"status\":\"online\",\"version\":\"1.0.0\"}";
                esp_mqtt_client_publish(s_mqtt_client, topic,
                                        status, 0, 1, 0);
            }
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected from broker");
            s_connected = false;
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGD(TAG, "MQTT subscribe acknowledged");
            break;

        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGD(TAG, "MQTT unsubscribe acknowledged");
            break;

        case MQTT_EVENT_PUBLISHED:
            ESP_LOGD(TAG, "MQTT publish acknowledged");
            break;

        case MQTT_EVENT_DATA: {
            if (event->topic == NULL || event->data == NULL) {
                ESP_LOGW(TAG, "MQTT_EVENT_DATA: topic or data is NULL");
                break;
            }

            /* Null-terminate the topic and data */
            char topic[MQTT_TOPIC_MAX];
            char data[MQTT_PAYLOAD_MAX];
            size_t topic_len = (size_t)event->topic_len < sizeof(topic) - 1
                               ? (size_t)event->topic_len : sizeof(topic) - 1;
            size_t data_len = (size_t)event->data_len < sizeof(data) - 1
                              ? (size_t)event->data_len : sizeof(data) - 1;
            memcpy(topic, event->topic, topic_len);
            topic[topic_len] = '\0';
            memcpy(data, event->data, data_len);
            data[data_len] = '\0';

            ESP_LOGI(TAG, "MQTT RX: %s => %s", topic, data);

            /* Check if this is a node command topic */
            if (strstr(topic, "/cmd") != NULL) {
                uint8_t node_id = parse_node_id_from_topic(topic);
                if (node_id > 0 && s_command_cb) {
                    s_command_cb(node_id, data, data_len);
                }
            }
            break;
        }

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error occurred");
            break;

        default:
            ESP_LOGD(TAG, "MQTT event: %ld", (long)event->event_id);
            break;
    }
}
