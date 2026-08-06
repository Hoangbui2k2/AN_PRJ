#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "lora_uart.h"
#include "node_manager.h"

static const char *TAG = "NODE_MGR";

static node_entry_t s_nodes[MAX_NODES];
static uint8_t s_node_count = 0;

void node_manager_init(void)
{
    memset(s_nodes, 0, sizeof(s_nodes));
    s_node_count = 0;
    ESP_LOGI(TAG, "Node manager initialized (max %d nodes)", MAX_NODES);
}

node_entry_t* node_find_or_create(uint8_t id)
{
    /* Try to find existing node */
    for (int i = 0; i < s_node_count; i++) {
        if (s_nodes[i].id == id) {
            return &s_nodes[i];
        }
    }

    /* Create new node if space available */
    if (s_node_count >= MAX_NODES) {
        ESP_LOGW(TAG, "Node table full, cannot add node 0x%02X", id);
        return NULL;
    }

    node_entry_t *node = &s_nodes[s_node_count];
    memset(node, 0, sizeof(node_entry_t));
    node->id = id;
    node->online = false;
    node->last_seen_ms = 0;
    node->threshold_low = THRESHOLD_LOW_DEFAULT;
    node->threshold_high = THRESHOLD_HIGH_DEFAULT;
    node->threshold_exceeded = false;
    node->delta_temp = DELTA_TEMP_DEFAULT;
    node->delta_hum = DELTA_HUM_DEFAULT;
    node->delta_soil = DELTA_SOIL_DEFAULT;
    node->delta_battery = DELTA_BATTERY_DEFAULT;
    node->report_interval = REPORT_INTERVAL_DEFAULT;
    node->heartbeat_interval = HEARTBEAT_INTERVAL_MS / 1000;
    node->schedule_hour = 6;
    node->schedule_minute = 0;
    node->mode = 1;
    s_node_count++;

    ESP_LOGI(TAG, "New node registered: 0x%02X (total: %d)", id, s_node_count);
    return node;
}

node_entry_t* node_find(uint8_t id)
{
    for (int i = 0; i < s_node_count; i++) {
        if (s_nodes[i].id == id) {
            return &s_nodes[i];
        }
    }
    return NULL;
}

bool node_register(uint8_t id, uint8_t threshold_low, uint8_t threshold_high,
                   uint8_t delta_temp, uint8_t delta_hum, uint8_t delta_soil,
                   uint8_t delta_battery, uint16_t report_interval,
                   uint16_t heartbeat_interval, uint8_t schedule_hour,
                   uint8_t schedule_minute, uint8_t mode)
{
    node_entry_t *node = node_find_or_create(id);
    if (!node) return false;

    node->threshold_low = threshold_low;
    node->threshold_high = threshold_high;
    node->delta_temp = delta_temp;
    node->delta_hum = delta_hum;
    node->delta_soil = delta_soil;
    node->delta_battery = delta_battery;
    node->report_interval = report_interval;
    node->heartbeat_interval = heartbeat_interval;
    node->schedule_hour = schedule_hour;
    node->schedule_minute = schedule_minute;
    node->mode = mode;

    ESP_LOGI(TAG, "Registered default node 0x%02X: Thr=[%d-%d] Delta=[t%d/h%d/s%d/b%d] "
             "Report=%d s Heartbeat=%d s Schedule=%02d:%02d Mode=%d",
             id, threshold_low, threshold_high, delta_temp, delta_hum,
             delta_soil, delta_battery, report_interval, heartbeat_interval,
             schedule_hour, schedule_minute, mode);
    return true;
}

uint64_t node_get_time_ms(void)
{
    return esp_timer_get_time() / 1000;
}

void node_update_data(uint8_t id, uint8_t soil, int8_t temp, uint8_t hum,
                      uint8_t battery, uint8_t pump_state, uint8_t flags)
{
    node_entry_t *node = node_find_or_create(id);
    if (!node) {
        ESP_LOGE(TAG, "Cannot update node 0x%02X: table full", id);
        return;
    }

    bool was_offline = !node->online;

    node->last_seen_ms = node_get_time_ms();
    node->soil = soil;
    node->temp = temp;
    node->hum = hum;
    node->battery = battery;
    node->pump_state = pump_state;
    node->flags = flags;
    node->online = true;

    ESP_LOGD(TAG, "Node 0x%02X updated: soil=%d temp=%d hum=%d batt=%d pump=%d",
             id, soil, temp, hum, battery, pump_state);

    if (was_offline) {
        ESP_LOGI(TAG, "Node 0x%02X is now ONLINE", id);
    }
}

uint16_t node_check_timeouts(void)
{
    uint16_t changed = 0;
    uint64_t now_ms = node_get_time_ms();

    for (int i = 0; i < s_node_count; i++) {
        if (!s_nodes[i].online) {
            continue; /* Already offline, skip */
        }

        uint64_t elapsed = now_ms - s_nodes[i].last_seen_ms;
        if (elapsed >= NODE_TIMEOUT_MS) {
            ESP_LOGI(TAG, "Node 0x%02X timeout: %llu ms since last data (threshold %d ms)",
                     s_nodes[i].id, elapsed, NODE_TIMEOUT_MS);
            s_nodes[i].online = false;
            changed |= (1 << i);
        }
    }

    return changed;
}

void node_mark_online(uint8_t id)
{
    node_entry_t *node = node_find_or_create(id);
    if (!node) return;

    if (!node->online) {
        ESP_LOGI(TAG, "Node 0x%02X marked ONLINE", id);
    }
    node->online = true;
    node->last_seen_ms = node_get_time_ms();
}

void node_mark_offline(uint8_t id)
{
    node_entry_t *node = node_find(id);
    if (!node) return;

    if (node->online) {
        ESP_LOGI(TAG, "Node 0x%02X marked OFFLINE", id);
    }
    node->online = false;
}

uint8_t node_get_count(void)
{
    return s_node_count;
}

node_entry_t* node_get_all(void)
{
    return s_nodes;
}

void node_print_all(void)
{
    ESP_LOGI(TAG, "===== Node Status (%d registered) =====", s_node_count);
    for (int i = 0; i < s_node_count; i++) {
        ESP_LOGI(TAG, "Node 0x%02X: %s | Soil=%d Temp=%d Hum=%d Batt=%d Pump=%d "
                 "Thr=[%d-%d]%s | Deltas=[t%d/h%d/s%d/b%d] | Rep=%ds Hb=%ds "
                 "Sch=%02d:%02d Mode=%d | Last: %llu",
                 s_nodes[i].id,
                 s_nodes[i].online ? "ONLINE " : "OFFLINE",
                 s_nodes[i].soil, s_nodes[i].temp, s_nodes[i].hum,
                 s_nodes[i].battery, s_nodes[i].pump_state,
                 s_nodes[i].threshold_low, s_nodes[i].threshold_high,
                 s_nodes[i].threshold_exceeded ? " EXCEEDED" : "",
                 s_nodes[i].delta_temp, s_nodes[i].delta_hum,
                 s_nodes[i].delta_soil, s_nodes[i].delta_battery,
                 s_nodes[i].report_interval, s_nodes[i].heartbeat_interval,
                 s_nodes[i].schedule_hour, s_nodes[i].schedule_minute,
                 s_nodes[i].mode,
                 s_nodes[i].last_seen_ms);
    }
    ESP_LOGI(TAG, "====================================");
}

/* ── Threshold-based reporting ───────────────────────────────────── */

void node_set_threshold(uint8_t id, uint8_t low, uint8_t high)
{
    node_entry_t *node = node_find_or_create(id);
    if (!node) return;

    /* Clamp to valid range */
    if (low > 100) low = 100;
    if (high > 100) high = 100;
    if (low >= high) {
        ESP_LOGW(TAG, "Invalid thresholds for node 0x%02X: low=%d >= high=%d", id, low, high);
        return;
    }

    node->threshold_low = low;
    node->threshold_high = high;
    ESP_LOGI(TAG, "Node 0x%02X thresholds: low=%d%% high=%d%%", id, low, high);
}

bool node_check_threshold(uint8_t id, uint8_t soil)
{
    node_entry_t *node = node_find(id);
    if (!node) return false;

    if (soil < node->threshold_low || soil > node->threshold_high) {
        node->threshold_exceeded = true;
        ESP_LOGD(TAG, "Node 0x%02X threshold EXCEEDED: soil=%d (band %d-%d)",
                 id, soil, node->threshold_low, node->threshold_high);
        return true;
    }

    node->threshold_exceeded = false;
    return false;
}

bool node_is_threshold_exceeded(uint8_t id)
{
    node_entry_t *node = node_find(id);
    if (!node) return false;
    return node->threshold_exceeded;
}

/* ── Delta threshold functions ─────────────────────────────── */

bool node_set_delta_threshold(uint8_t id, uint8_t type, uint8_t value)
{
    node_entry_t *node = node_find(id);
    if (!node) return false;

    switch (type) {
        case DELTA_TYPE_TEMPERATURE:
            node->delta_temp = value;
            ESP_LOGI(TAG, "Node 0x%02X delta temp: %d (%.1f°C)", id, value, value / 10.0f);
            return true;
        case DELTA_TYPE_HUMIDITY:
            node->delta_hum = value;
            ESP_LOGI(TAG, "Node 0x%02X delta hum: %d%%", id, value);
            return true;
        case DELTA_TYPE_SOIL:
            node->delta_soil = value;
            ESP_LOGI(TAG, "Node 0x%02X delta soil: %d%%", id, value);
            return true;
        case DELTA_TYPE_BATTERY:
            node->delta_battery = value;
            ESP_LOGI(TAG, "Node 0x%02X delta battery: %d%%", id, value);
            return true;
        default:
            ESP_LOGW(TAG, "Unknown delta type: %d for node 0x%02X", type, id);
            return false;
    }
}

uint8_t node_get_delta_threshold(uint8_t id, uint8_t type)
{
    node_entry_t *node = node_find(id);
    if (!node) return 0;

    switch (type) {
        case DELTA_TYPE_TEMPERATURE: return node->delta_temp;
        case DELTA_TYPE_HUMIDITY:    return node->delta_hum;
        case DELTA_TYPE_SOIL:        return node->delta_soil;
        case DELTA_TYPE_BATTERY:     return node->delta_battery;
        default:                     return 0;
    }
}

void node_update_last_reported(uint8_t id)
{
    node_entry_t *node = node_find(id);
    if (!node) return;

    node->last_reported_temp = node->temp;
    node->last_reported_hum = node->hum;
    node->last_reported_soil = node->soil;
    node->last_reported_battery = node->battery;

    ESP_LOGD(TAG, "Node 0x%02X last_reported updated", id);
}
