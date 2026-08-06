/*
 * ESP32 LoRa-to-MQTT Gateway
 * ===========================
 * 
 * Firmware for an ESP32-based gateway that bridges LoRa UART to MQTT.
 * Manages up to 10 irrigation nodes, caches commands for offline nodes,
 * and retries failed commands with exponential backoff.
 *
 * Hardware:
 *   - ESP32 DevKit V1
 *   - SX1278 UART LoRa Module (UART2: GPIO16/17, MD0/MD1: GPIO23/22, AUX: GPIO19)
 *
 * ESP-IDF Framework
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "cJSON.h"

#include "lora_uart.h"
#include "mqtt.h"
#include "node_manager.h"
#include "command_cache.h"
#include "config.h"
#include "topic.h"
#include "crc.h"

/* ---------------------------- Constants ----------------------------------- */

#define TAG "GATEWAY"

/* WiFi connection timeout */
#define WIFI_CONNECT_TIMEOUT_MS  30000

/* Task stack sizes and priorities */
#define LORA_RX_STACK_SIZE       4096
#define LORA_RX_PRIORITY         5
#define NODE_MONITOR_STACK_SIZE  3072
#define NODE_MONITOR_PRIORITY    4
#define CMD_RETRY_STACK_SIZE     3072
#define CMD_RETRY_PRIORITY       4

/* Task periods */
#define NODE_MONITOR_PERIOD_MS   1000   /* Check node timeouts every 1s */
#define CMD_RETRY_PERIOD_MS      500    /* Check retry queue every 500ms */
#define GATEWAY_STATUS_INTERVAL_MS 60000 /* Publish gateway status every 60s */

/* ---------------------------- Global State -------------------------------- */

static EventGroupHandle_t s_wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;
static const int WIFI_FAIL_BIT      = BIT1;

static gateway_config_t s_config;
static int s_retry_num = 0;

/* ---------------------------- WiFi Management ---------------------------- */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "WiFi: attempting to connect...");
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < 5) {
            ESP_LOGW(TAG, "WiFi disconnected, reconnecting... (attempt %d)", s_retry_num + 1);
            esp_wifi_connect();
            s_retry_num++;
        } else {
            ESP_LOGE(TAG, "WiFi connection failed after %d attempts", s_retry_num);
            /* Exponential backoff for reconnect: will retry in event loop */
            vTaskDelay(pdMS_TO_TICKS(5000 << (s_retry_num - 4))); /* 5s, 10s, 20s... */
            s_retry_num = 0;
            esp_wifi_connect();
        }
    } 
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    assert(netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Register event handlers */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, 
                                                &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, 
                                                &wifi_event_handler, NULL));

    /* Configure WiFi */
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };
    strlcpy((char*)wifi_config.sta.ssid, s_config.wifi_ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char*)wifi_config.sta.password, s_config.wifi_password, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi station initialized, connecting to SSID: %s", s_config.wifi_ssid);

    /* Wait for connection or timeout */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected successfully");
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "WiFi connection timeout");
        return ESP_FAIL;
    }
}

/* ---------------------------- JSON Payload Construction ------------------- */

static void publish_node_data(node_entry_t *node)
{
    if (!node || !mqtt_is_connected()) return;

    char topic[TOPIC_MAX_LEN];
    topic_build(topic, sizeof(topic), s_config.site, s_config.gateway_id,
                TOPIC_NODE_DATA, node->id);

    cJSON *root = cJSON_CreateObject();
    if (!root) return;

    cJSON_AddNumberToObject(root, "node", node->id);
    cJSON_AddNumberToObject(root, "soil", node->soil);
    cJSON_AddNumberToObject(root, "temp", node->temp);
    cJSON_AddNumberToObject(root, "hum", node->hum);
    cJSON_AddNumberToObject(root, "pump", node->pump_state);
    cJSON_AddNumberToObject(root, "battery", node->battery);

    /* Threshold exceeded flag */
    cJSON_AddBoolToObject(root, "threshold_exceeded", node->threshold_exceeded);
    if (node->threshold_exceeded) {
        cJSON_AddNumberToObject(root, "threshold_low", node->threshold_low);
        cJSON_AddNumberToObject(root, "threshold_high", node->threshold_high);
    }

    /* Add ISO 8601 timestamp */
    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
    cJSON_AddStringToObject(root, "timestamp", timestamp);

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        mqtt_publish(topic, json_str, 1);
        ESP_LOGI(TAG, "Published data for node 0x%02X: %s", node->id, json_str);
        free(json_str);
    }

    cJSON_Delete(root);
}

/* ── Alarm code mapping ──────────────────────────────────────────── */

static const char* alarm_code_to_message(uint8_t code)
{
    switch (code) {
        case 0x01: return "Sensor error — 3 consecutive sensor read failures";
        case 0x02: return "Soil out of range — soil moisture < 10% or > 90%";
        case 0x03: return "Relay error — pump relay fault (pulse or GPIO)";
        case 0x04: return "Low battery — battery < 20%";
        case 0x05: return "Gateway lost — node lost connection (>=3 ACK timeouts)";
        default:   return "Unknown alarm";
    }
}

static void publish_node_alarm_code(uint8_t node_id, uint8_t alarm_code, uint8_t flags)
{
    if (!mqtt_is_connected()) return;

    char topic[TOPIC_MAX_LEN];
    topic_build(topic, sizeof(topic), s_config.site, s_config.gateway_id,
                TOPIC_NODE_ALARM, node_id);

    cJSON *root = cJSON_CreateObject();
    if (!root) return;

    cJSON_AddNumberToObject(root, "node", node_id);
    cJSON_AddNumberToObject(root, "alarm_code", alarm_code);
    cJSON_AddStringToObject(root, "type", "alarm");
    cJSON_AddStringToObject(root, "message", alarm_code_to_message(alarm_code));
    cJSON_AddNumberToObject(root, "flags", flags);

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        mqtt_publish(topic, json_str, 1);
        ESP_LOGI(TAG, "Published alarm_code=0x%02X for node 0x%02X: %s",
                 alarm_code, node_id, json_str);
        free(json_str);
    }

    cJSON_Delete(root);
}

static void publish_node_alarm(node_entry_t *node)
{
    if (!node || !mqtt_is_connected()) return;

    char topic[TOPIC_MAX_LEN];
    topic_build(topic, sizeof(topic), s_config.site, s_config.gateway_id,
                TOPIC_NODE_ALARM, node->id);

    cJSON *root = cJSON_CreateObject();
    if (!root) return;

    cJSON_AddNumberToObject(root, "node", node->id);
    cJSON_AddNumberToObject(root, "soil", node->soil);
    cJSON_AddStringToObject(root, "type", "alarm");
    cJSON_AddStringToObject(root, "message", "Sensor alarm triggered");

    /* Include threshold context if exceeded */
    if (node->threshold_exceeded) {
        cJSON_AddNumberToObject(root, "threshold_low", node->threshold_low);
        cJSON_AddNumberToObject(root, "threshold_high", node->threshold_high);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        mqtt_publish(topic, json_str, 1);
        ESP_LOGI(TAG, "Published alarm for node 0x%02X: %s", node->id, json_str);
        free(json_str);
    }

    cJSON_Delete(root);
}

static void publish_node_status(uint8_t node_id, bool online)
{
    if (!mqtt_is_connected()) return;

    char topic[TOPIC_MAX_LEN];
    topic_build(topic, sizeof(topic), s_config.site, s_config.gateway_id,
                TOPIC_NODE_STATUS, node_id);

    cJSON *root = cJSON_CreateObject();
    if (!root) return;

    cJSON_AddNumberToObject(root, "node", node_id);
    cJSON_AddStringToObject(root, "status", online ? "online" : "offline");
    cJSON_AddNumberToObject(root, "timestamp", (double)(node_get_time_ms() / 1000));

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        mqtt_publish(topic, json_str, 1);
        ESP_LOGI(TAG, "Published status for node 0x%02X: %s", node_id, json_str);
        free(json_str);
    }

    cJSON_Delete(root);
}

/* ---------------------------- LoRa Packet Processing ---------------------- */

/**
 * @brief Send all pending cached commands for a node
 *
 * Called when a node becomes reachable (data / heartbeat / alarm packet).
 * Sends commands in FIFO order, one at a time — each sent command enters
 * the ACK-wait state and is skipped until it is ACKed or times out.
 */
static void send_cached_commands_for_node(uint8_t node_id)
{
    cached_command_t *cached;
    while ((cached = command_cache_get_next(node_id)) != NULL) {
        ESP_LOGI(TAG, "Sending cached command to node 0x%02X (retry %d/%d)",
                 node_id, cached->retry_count, MAX_RETRY);

        if (lora_send_command(cached->cmd[0], cached->cmd[2],
                              cached->cmd[3], cached->cmd[4])) {
            command_cache_mark_sent(cached);
        } else {
            ESP_LOGW(TAG, "Failed to send cached command to node 0x%02X", node_id);
            break; /* Stop sending until next opportunity */
        }
    }
}

/**
 * @brief Respond to an uplink from a node: flush commands or send an empty ACK
 *
 * Every uplink packet (data / heartbeat / alarm) expects a downlink response
 * so the node can reset its gatewayLostCount. If there are queued commands
 * for the node, they are flushed now and double as the response. Otherwise an
 * empty ACK (LORA_CMD_ACK) is sent so the node does not sit waiting for one.
 */
static void ack_or_flush_node(uint8_t node_id)
{
    if (command_cache_count_for_node(node_id) > 0) {
        send_cached_commands_for_node(node_id);
    } else {
        if (lora_send_ack(node_id)) {
            ESP_LOGD(TAG, "Sent empty ACK to node 0x%02X (no command queued)", node_id);
        }
    }
}

static void process_node_packet(const lora_uplink_packet_t *pkt)
{
    ESP_LOGI(TAG, "Processing uplink: Node=0x%02X Type=0x%02X Flags=0x%02X "
             "Soil=%d Temp=%d Hum=%d Batt=%d",
             pkt->node_id, pkt->type, pkt->flags,
             pkt->soil, pkt->temp, pkt->hum, pkt->battery);

    /* Handle ACK packets (type 0x03) */
    if (pkt->type == PKT_TYPE_ACK) {
        ESP_LOGI(TAG, "ACK received from node 0x%02X", pkt->node_id);
        /* We can't match the exact command bytes in an ACK packet, so we
         * acknowledge the in-flight command (the one waiting for this ACK). */
        cached_command_t *cmd = command_cache_get_inflight(pkt->node_id);
        if (cmd) {
            command_cache_remove(cmd);
            ESP_LOGI(TAG, "Command ACKed and removed for node 0x%02X", pkt->node_id);
        } else {
            ESP_LOGW(TAG, "Unexpected ACK from node 0x%02X — no command in flight",
                     pkt->node_id);
        }
        return;
    }

    /* Handle Heartbeat packets (type 0x02) */
    if (pkt->type == PKT_TYPE_HEARTBEAT) {
        ESP_LOGI(TAG, "Heartbeat from node 0x%02X (battery=%d)", pkt->node_id, pkt->battery);

        bool was_offline = false;
        node_entry_t *n = node_find(pkt->node_id);
        if (!n || !n->online) {
            was_offline = true;  /* New node, or was offline */
        }

        /* Mark node online and update last_seen */
        node_mark_online(pkt->node_id);

        /* Respond so the node can reset its gatewayLostCount: flush any
         * cached commands, or send an empty ACK if there is nothing queued. */
        ack_or_flush_node(pkt->node_id);

        if (was_offline) {
            publish_node_status(pkt->node_id, true);
        }
        return;
    }

    /* ── Handle Alarm packets (type 0x04) ──
     *
     * Alarm packets are fire-and-forget (no ACK). The alarm_code is sent
     * in BOTH the flags byte (byte 2) and soil byte (byte 3) for redundancy.
     * We cross-verify them as a sanity check before processing.
     */
    if (pkt->type == PKT_TYPE_ALARM) {
        uint8_t alarm_code = pkt->flags;

        /* Cross-verify alarm_code from flags vs soil (redundant copy) */
        if (alarm_code != pkt->soil) {
            ESP_LOGW(TAG, "Alarm code mismatch: flags=0x%02X soil=0x%02X, using flags",
                     alarm_code, pkt->soil);
        }

        if (alarm_code >= 0x01 && alarm_code <= 0x05) {
            ESP_LOGI(TAG, "ALARM [0x%02X] from node 0x%02X: %s",
                     alarm_code, pkt->node_id, alarm_code_to_message(alarm_code));
        } else {
            ESP_LOGW(TAG, "Unknown alarm code 0x%02X from node 0x%02X",
                     alarm_code, pkt->node_id);
        }

        /* Check if this alarm marks a node coming back online */
        bool was_offline = false;
        node_entry_t *n = node_find(pkt->node_id);
        if (!n || !n->online) {
            was_offline = true;  /* New node, or was offline */
        }

        /* Update node online status only — do NOT update sensor data */
        node_mark_online(pkt->node_id);

        /* Respond so the node can reset its gatewayLostCount: flush any
         * cached commands, or send an empty ACK if there is nothing queued. */
        ack_or_flush_node(pkt->node_id);

        /* Publish alarm with proper alarm_code to MQTT */
        publish_node_alarm_code(pkt->node_id, alarm_code, pkt->flags);

        /* If node just came online, notify server */
        if (was_offline) {
            publish_node_status(pkt->node_id, true);
        }

        return; /* Skip data processing for alarm packets */
    }

    /* ── Update node data ── */
    bool was_offline = false;
    node_entry_t *node = node_find(pkt->node_id);
    if (node && !node->online) {
        was_offline = true;
    }

    uint8_t pump_state;
    bool threshold_flag_from_node;

    if (pkt->type == PKT_TYPE_DATA_COMPACT) {
        /* Compact packet: merge with existing last_data, keep old values for absent fields */
        node_entry_t *n = node_find_or_create(pkt->node_id);
        if (n) {
            uint8_t soil   = n->soil;
            int8_t temp    = n->temp;
            uint8_t hum    = n->hum;
            uint8_t bat    = n->battery;
            uint8_t flags  = n->flags;
            pump_state     = n->pump_state;

            if (pkt->presence & PRESENCE_TEMPERATURE) temp = pkt->temp;
            if (pkt->presence & PRESENCE_HUMIDITY)    hum  = pkt->hum;
            if (pkt->presence & PRESENCE_SOIL_MOIST)  soil = pkt->soil;
            if (pkt->presence & PRESENCE_BATTERY)     bat  = pkt->battery;
            if (pkt->presence & PRESENCE_FLAGS) {
                flags       = pkt->flags;
                pump_state  = (pkt->flags & FLAG_PUMP_ON) ? 1 : 0;
            }

            threshold_flag_from_node = (flags & FLAG_THRESHOLD_EXCEEDED) ? true : false;

            node_update_data(pkt->node_id, soil, temp, hum, bat, pump_state, flags);

            ESP_LOGD(TAG, "Compact merge node 0x%02X: "
                     "soil=%d temp=%d hum=%d batt=%d pump=%d flags=0x%02X",
                     pkt->node_id, soil, temp, hum, bat, pump_state, flags);
        } else {
            /* Cannot find or create node — fallback: direct update */
            pump_state = (pkt->flags & FLAG_PUMP_ON) ? 1 : 0;
            threshold_flag_from_node = (pkt->flags & FLAG_THRESHOLD_EXCEEDED) ? true : false;
            node_update_data(pkt->node_id, pkt->soil, pkt->temp, pkt->hum,
                             pkt->battery, pump_state, pkt->flags);
        }
    } else {
        /* Legacy packet (0x01, 0x02): all fields present */
        pump_state = (pkt->flags & FLAG_PUMP_ON) ? 1 : 0;
        threshold_flag_from_node = (pkt->flags & FLAG_THRESHOLD_EXCEEDED) ? true : false;

        node_update_data(pkt->node_id, pkt->soil, pkt->temp, pkt->hum,
                         pkt->battery, pump_state, pkt->flags);
    }

    /* Re-fetch node after update */
    node = node_find(pkt->node_id);
    if (!node) return;

    /* If node just came online, notify the server */
    if (was_offline) {
        ESP_LOGI(TAG, "Node 0x%02X came online", pkt->node_id);
        publish_node_status(pkt->node_id, true);
    }

    /* Check threshold — combine node's hardware detection with gateway's verification */
    bool exceeded = node_check_threshold(pkt->node_id, node->soil);

    /* Publish data to MQTT (always) */
    publish_node_data(node);

    /* Update last_reported values after successful publication */
    node_update_last_reported(pkt->node_id);

    /* If threshold flag from node OR gateway verification exceeded:
       publish alarm so the server is notified */
    if ((exceeded || threshold_flag_from_node) && pkt->type != PKT_TYPE_ALARM) {
        ESP_LOGI(TAG, "Node 0x%02X threshold EXCEEDED (node flag=%d, gw check=%d), alarm sent",
                 pkt->node_id, threshold_flag_from_node, exceeded);
        publish_node_alarm(node);
    }

    /* Log delta-triggered data (when threshold flag NOT set, it came from delta change) */
    if (!exceeded && !threshold_flag_from_node && pkt->type == PKT_TYPE_DATA) {
        ESP_LOGD(TAG, "Node 0x%02X data triggered by delta change", pkt->node_id);
    }

    /* Respond so the node can reset its gatewayLostCount: flush any cached
     * commands, or send an empty ACK if there is nothing queued. This makes
     * every data uplink get a downlink reply, so the node never sits waiting
     * for an ACK it will not receive. */
    ack_or_flush_node(pkt->node_id);
}

/* ---------------------------- MQTT Command Processing --------------------- */

/**
 * @brief Map JSON command string to LoRa command byte
 */
static uint8_t map_command_to_lora_byte(const char *cmd_str)
{
    if (strcmp(cmd_str, "set_interval") == 0)   return LORA_CMD_SET_INTERVAL;
    if (strcmp(cmd_str, "on") == 0)              return LORA_CMD_ON;
    if (strcmp(cmd_str, "off") == 0)             return LORA_CMD_OFF;
    if (strcmp(cmd_str, "set_threshold") == 0)   return LORA_CMD_SET_THRESHOLD;
    if (strcmp(cmd_str, "set_schedule") == 0)    return LORA_CMD_SET_SCHEDULE;
    if (strcmp(cmd_str, "report") == 0)          return LORA_CMD_REPORT;
    if (strcmp(cmd_str, "toggle") == 0)          return LORA_CMD_TOGGLE;
    if (strcmp(cmd_str, "set_mode") == 0)        return LORA_CMD_SET_MODE;
    if (strcmp(cmd_str, "set_delta") == 0)       return LORA_CMD_SET_DELTA;
    return 0;
}

/**
 * @brief Get the default values for a node, used when a server command
 *        omits a parameter.
 *
 * Prefers the live node state (if registered); otherwise falls back to the
 * matching entry in the startup config (default 2 nodes).
 */
static void get_node_defaults(uint8_t node_id, node_config_t *out)
{
    node_entry_t *node = node_find(node_id);
    if (node) {
        out->id = node->id;
        out->threshold_low = node->threshold_low;
        out->threshold_high = node->threshold_high;
        out->delta_temp = node->delta_temp;
        out->delta_hum = node->delta_hum;
        out->delta_soil = node->delta_soil;
        out->delta_battery = node->delta_battery;
        out->report_interval = node->report_interval;
        out->heartbeat_interval = node->heartbeat_interval;
        out->schedule_hour = node->schedule_hour;
        out->schedule_minute = node->schedule_minute;
        out->mode = node->mode;
        return;
    }
    /* Not registered yet: fall back to startup config (match by id, else first) */
    for (int i = 0; i < DEFAULT_NODE_COUNT; i++) {
        if (s_config.nodes[i].id == node_id) {
            *out = s_config.nodes[i];
            return;
        }
    }
    *out = s_config.nodes[0];
}

static void handle_mqtt_command(uint8_t node_id, const char *payload, size_t len)
{
    (void)len;

    ESP_LOGI(TAG, "Processing MQTT command for node 0x%02X: %s", node_id, payload);

    cJSON *root = cJSON_Parse(payload);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse JSON command: %s", payload);
        return;
    }

    cJSON *cmd_item = cJSON_GetObjectItem(root, "cmd");
    if (!cmd_item || !cJSON_IsString(cmd_item)) {
        ESP_LOGW(TAG, "JSON command missing 'cmd' field");
        cJSON_Delete(root);
        return;
    }

    const char *cmd_str = cmd_item->valuestring;
    uint8_t lora_cmd_byte = map_command_to_lora_byte(cmd_str);

    if (lora_cmd_byte == 0) {
        ESP_LOGW(TAG, "Unknown command: %s", cmd_str);
        cJSON_Delete(root);
        return;
    }

    /* Defaults from node config — used when the server omits a parameter */
    node_config_t defaults;
    get_node_defaults(node_id, &defaults);

    /* Build LoRa command parameters based on command type */
    uint8_t param1 = 0;
    uint8_t param2 = 0;

    switch (lora_cmd_byte) {
        case LORA_CMD_SET_INTERVAL: {
            cJSON *val = cJSON_GetObjectItem(root, "value");
            if (val && cJSON_IsNumber(val)) {
                param1 = (uint8_t)(val->valueint & 0xFF);
            } else {
                /* Default: node's configured report interval */
                param1 = (uint8_t)(defaults.report_interval & 0xFF);
            }
            break;
        }
        case LORA_CMD_ON: {
            cJSON *dur = cJSON_GetObjectItem(root, "duration");
            if (dur && cJSON_IsNumber(dur)) {
                param1 = (uint8_t)(dur->valueint & 0xFF);
            }
            break;
        }
        case LORA_CMD_OFF: {
            /* No parameters needed */
            break;
        }
        case LORA_CMD_SET_THRESHOLD: {
            cJSON *low = cJSON_GetObjectItem(root, "low");
            cJSON *high = cJSON_GetObjectItem(root, "high");
            uint8_t low_val = (low && cJSON_IsNumber(low)) ? (uint8_t)(low->valueint & 0xFF) : defaults.threshold_low;
            uint8_t high_val = (high && cJSON_IsNumber(high)) ? (uint8_t)(high->valueint & 0xFF) : defaults.threshold_high;
            param1 = low_val;
            param2 = high_val;

            /* Also update local threshold cache immediately so checks work
               before the node LoRa ACK comes back */
            node_set_threshold(node_id, low_val, high_val);
            break;
        }
        case LORA_CMD_SET_SCHEDULE: {
            cJSON *hour = cJSON_GetObjectItem(root, "hour");
            cJSON *min = cJSON_GetObjectItem(root, "minute");
            param1 = (hour && cJSON_IsNumber(hour)) ? (uint8_t)(hour->valueint & 0xFF) : defaults.schedule_hour;
            param2 = (min && cJSON_IsNumber(min)) ? (uint8_t)(min->valueint & 0xFF) : defaults.schedule_minute;
            break;
        }
        case LORA_CMD_REPORT: {
            /* No parameters needed */
            break;
        }
        case LORA_CMD_TOGGLE: {
            /* No parameters needed */
            break;
        }
        case LORA_CMD_SET_MODE: {
            cJSON *mode = cJSON_GetObjectItem(root, "mode");
            if (mode && cJSON_IsNumber(mode)) {
                param1 = (uint8_t)(mode->valueint & 0xFF);
            } else {
                param1 = defaults.mode;
            }
            break;
        }
        case LORA_CMD_SET_DELTA: {
            cJSON *type_item = cJSON_GetObjectItem(root, "type");
            cJSON *value_item = cJSON_GetObjectItem(root, "value");
            uint8_t dtype = (type_item && cJSON_IsNumber(type_item))
                                ? (uint8_t)(type_item->valueint & 0xFF)
                                : DELTA_TYPE_SOIL;
            uint8_t dvalue;
            if (value_item && cJSON_IsNumber(value_item)) {
                dvalue = (uint8_t)(value_item->valueint & 0xFF);
            } else {
                /* Default: node's configured delta for this type */
                switch (dtype) {
                    case DELTA_TYPE_TEMPERATURE: dvalue = defaults.delta_temp;     break;
                    case DELTA_TYPE_HUMIDITY:    dvalue = defaults.delta_hum;      break;
                    case DELTA_TYPE_SOIL:        dvalue = defaults.delta_soil;     break;
                    case DELTA_TYPE_BATTERY:     dvalue = defaults.delta_battery;  break;
                    default:                     dvalue = defaults.delta_soil;     break;
                }
            }
            param1 = dtype;
            param2 = dvalue;
            /* Also update local cache immediately */
            node_set_delta_threshold(node_id, param1, param2);
            break;
        }
        default:
            break;
    }

    cJSON_Delete(root);

    /* Check if node is online */
    node_entry_t *node = node_find(node_id);

    if (node && node->online) {
        /* Send command directly with retry */
        ESP_LOGI(TAG, "Node 0x%02X online, sending command 0x%02X", node_id, lora_cmd_byte);

        bool sent = false;
        for (int attempt = 0; attempt < MAX_RETRY; attempt++) {
            if (lora_send_command(node_id, lora_cmd_byte, param1, param2)) {
                sent = true;
                ESP_LOGI(TAG, "Command 0x%02X sent to node 0x%02X (attempt %d/%d)",
                         lora_cmd_byte, node_id, attempt + 1, MAX_RETRY);
                break;
            }
            /* Exponential backoff: 1s, 2s, 4s, 8s, 16s */
            int delay_ms = 1000 << attempt;
            if (delay_ms > 16000) delay_ms = 16000;
            ESP_LOGW(TAG, "Send attempt %d/%d failed, retrying in %d ms",
                     attempt + 1, MAX_RETRY, delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }

        if (!sent) {
            ESP_LOGW(TAG, "Failed to send command to node 0x%02X after %d attempts, caching",
                     node_id, MAX_RETRY);
            /* Build the raw command packet for caching */
            uint8_t cmd_pkt[6];
            cmd_pkt[0] = node_id;
            cmd_pkt[1] = LORA_CMD_HEADER;
            cmd_pkt[2] = lora_cmd_byte;
            cmd_pkt[3] = param1;
            cmd_pkt[4] = param2;
            cmd_pkt[5] = crc8_calculate(cmd_pkt, 5);
            command_cache_add(node_id, cmd_pkt);
        }
    } else {
        /* Node offline, cache the command */
        ESP_LOGW(TAG, "Node 0x%02X offline, caching command 0x%02X", node_id, lora_cmd_byte);
        
        uint8_t cmd_pkt[6];
        cmd_pkt[0] = node_id;
        cmd_pkt[1] = LORA_CMD_HEADER;
        cmd_pkt[2] = lora_cmd_byte;
        cmd_pkt[3] = param1;
        cmd_pkt[4] = param2;
        cmd_pkt[5] = crc8_calculate(cmd_pkt, 5);
        command_cache_add(node_id, cmd_pkt);
    }
}

/* ---------------------------- FreeRTOS Tasks ------------------------------ */

/**
 * @brief LoRa RX Task
 * 
 * Continuously reads from UART for LoRa packets.
 * Processes valid packets and publishes to MQTT.
 * Also checks for node timeouts periodically.
 */
static void lora_rx_task(void *pvParameters)
{
    (void)pvParameters;

    lora_uplink_packet_t packet;
    uint64_t last_timeout_check = 0;

    ESP_LOGI(TAG, "LoRa RX task started");

    while (1) {
        /* Try to read a packet (non-blocking) */
        if (lora_read_packet(&packet)) {
            /* Valid packet received, process it */
            if (packet.type == PKT_TYPE_DATA_COMPACT) {
                ESP_LOGI(TAG, "LoRa RX compact: node=0x%02X type=0x%02X "
                         "presence=0x%02X soil=%d temp=%d hum=%d batt=%d "
                         "flags=0x%02X crc=0x%02X",
                         packet.node_id, packet.type, packet.presence,
                         packet.soil, packet.temp, packet.hum, packet.battery,
                         packet.flags, packet.crc);
            } else {
                ESP_LOGI(TAG, "LoRa RX: %02X %02X %02X %02X %02X %02X %02X %02X",
                         packet.node_id, packet.type, packet.flags,
                         packet.soil, packet.temp, packet.hum, packet.battery, packet.crc);
            }

            process_node_packet(&packet);
        }

        /* Check node timeouts every 1 second */
        uint64_t now_ms = node_get_time_ms();
        if ((now_ms - last_timeout_check) >= NODE_MONITOR_PERIOD_MS) {
            last_timeout_check = now_ms;
            
            uint16_t changed = node_check_timeouts();
            if (changed) {
                /* Publish status for nodes that timed out */
                for (int i = 0; i < MAX_NODES; i++) {
                    if (changed & (1 << i)) {
                        node_entry_t *node = node_get_all() + i;
                        ESP_LOGI(TAG, "Node 0x%02X went OFFLINE (timeout)", node->id);
                        publish_node_status(node->id, false);
                    }
                }
            }

            /* Log node status every 30 seconds */
            static uint64_t last_status_log = 0;
            if ((now_ms - last_status_log) >= 30000) {
                last_status_log = now_ms;
                node_print_all();
                command_cache_print();
            }
        }

        /* Yield to other tasks */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * @brief Command Retry Task
 * 
 * Periodically checks the command cache for commands that need retry.
 * Sends them if the node is now online.
 */
static void cmd_retry_task(void *pvParameters)
{
    (void)pvParameters;

    ESP_LOGI(TAG, "Command retry task started");

    while (1) {
        cached_command_t *cmd = command_cache_get_retry_ready();
        if (cmd) {
            node_entry_t *node = node_find(cmd->node_id);

            if (node && node->online) {
                ESP_LOGI(TAG, "Retrying cached command for node 0x%02X (attempt %d/%d)",
                         cmd->node_id, cmd->retry_count + 1, MAX_RETRY);

                if (lora_send_command(cmd->cmd[0], cmd->cmd[2], 
                                      cmd->cmd[3], cmd->cmd[4])) {
                    /* Wait for node ACK before considering it delivered */
                    command_cache_mark_sent(cmd);
                } else {
                    ESP_LOGW(TAG, "Retry send failed for node 0x%02X", cmd->node_id);
                    command_cache_advance_retry(cmd);
                }
            } else {
                /* Node still offline, advance retry timer */
                command_cache_advance_retry(cmd);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(CMD_RETRY_PERIOD_MS));
    }
}

/**
 * @brief Gateway Status Task
 * 
 * Periodically publishes gateway health status to MQTT.
 */
static void gateway_status_task(void *pvParameters)
{
    (void)pvParameters;

    ESP_LOGI(TAG, "Gateway status task started");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(GATEWAY_STATUS_INTERVAL_MS));

        if (mqtt_is_connected()) {
            cJSON *root = cJSON_CreateObject();
            if (root) {
                cJSON_AddStringToObject(root, "status", "online");
                cJSON_AddNumberToObject(root, "nodes_registered", node_get_count());
                cJSON_AddNumberToObject(root, "commands_cached", command_cache_total_pending());
                cJSON_AddNumberToObject(root, "uptime_seconds", 
                                        (double)(node_get_time_ms() / 1000));
                cJSON_AddStringToObject(root, "version", "1.0.0");
                cJSON_AddStringToObject(root, "site", s_config.site);
                cJSON_AddStringToObject(root, "gateway_id", s_config.gateway_id);

                char *json_str = cJSON_PrintUnformatted(root);
                if (json_str) {
                    char topic[TOPIC_MAX_LEN];
                    topic_build(topic, sizeof(topic),
                                s_config.site, s_config.gateway_id,
                                TOPIC_GW_STATUS, 0);
                    mqtt_publish(topic, json_str, 1);
                    free(json_str);
                }
                cJSON_Delete(root);
            }
        }
    }
}

/* ---------------------------- Main Entry Point ---------------------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  ESP32 LoRa-to-MQTT Gateway v1.0.0");
    ESP_LOGI(TAG, "========================================");

    /* Initialize NVS and load configuration */
    ESP_ERROR_CHECK(config_init(&s_config));
    config_print(&s_config);

    /* Initialize TCP/IP stack */
    esp_netif_init();

    /* Initialize default event loop */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Initialize WiFi in station mode */
    esp_err_t wifi_ret = wifi_init_sta();
    if (wifi_ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi not connected, will retry in background. "
                 "MQTT features will be unavailable until WiFi connects.");
        /* Continue without WiFi - the system will auto-reconnect */
    }

    /* Initialize MQTT client (will auto-connect when WiFi is ready) */
    esp_err_t mqtt_ret = mqtt_app_init(s_config.mqtt_broker_uri,
                                        s_config.mqtt_username,
                                        s_config.mqtt_password,
                                        s_config.mqtt_port,
                                        s_config.site,
                                        s_config.gateway_id);
    if (mqtt_ret != ESP_OK) {
        ESP_LOGW(TAG, "MQTT initialization failed, will retry later");
    }

    /* Set MQTT command callback */
    mqtt_set_command_callback(handle_mqtt_command);

    /* Initialize LoRa UART module */
    ESP_ERROR_CHECK(lora_uart_init());

    /* Initialize node manager and command cache */
    node_manager_init();
    command_cache_init();

    /* Register the 2 default nodes from config */
    for (int i = 0; i < DEFAULT_NODE_COUNT; i++) {
        node_config_t *nc = &s_config.nodes[i];
        node_register(nc->id, nc->threshold_low, nc->threshold_high,
                      nc->delta_temp, nc->delta_hum, nc->delta_soil,
                      nc->delta_battery, nc->report_interval,
                      nc->heartbeat_interval, nc->schedule_hour,
                      nc->schedule_minute, nc->mode);
    }

    /* Give some time for modules to stabilize */
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Create FreeRTOS tasks */
    TaskHandle_t lora_rx_handle = NULL;
    TaskHandle_t cmd_retry_handle = NULL;
    TaskHandle_t gw_status_handle = NULL;

    xTaskCreate(lora_rx_task, "lora_rx", LORA_RX_STACK_SIZE, NULL, 
                LORA_RX_PRIORITY, &lora_rx_handle);
    xTaskCreate(cmd_retry_task, "cmd_retry", CMD_RETRY_STACK_SIZE, NULL, 
                CMD_RETRY_PRIORITY, &cmd_retry_handle);
    xTaskCreate(gateway_status_task, "gw_status", LORA_RX_STACK_SIZE, NULL, 
                LORA_RX_PRIORITY, &gw_status_handle);

    ESP_LOGI(TAG, "Gateway initialization complete. All tasks running.");

    /* Main loop - handle any remaining work */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        
        /* Log memory status periodically */
        ESP_LOGD(TAG, "Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());
    }
}
