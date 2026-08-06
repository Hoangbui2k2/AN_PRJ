#include "commands.h"
#include "lora_uart.h"
#include "config.h"
#include "sensors.h"
#include "pump.h"
#include "power.h"
#include "irrigation.h"
#include "crc.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <string.h>

static const char *TAG = "COMMANDS";

/* ──────────── Gateway Protocol Timing ──────────── */
#define UPLINK_RETRIES        3   /* Max uplink transmit attempts */
#define ACK_SEND_RETRIES      3   /* ACK transmit retries (target < 500 ms) */
#define PENDING_WINDOW_MS     250 /* Listen window when no uplink was just sent */

/* ──────────── Command Deduplication (RTC-persistent) ────────────
 *
 * The gateway sends ONE command at a time and retries (backoff 1s→16s, max 5)
 * until it receives ACK.  If the node's ACK is lost in RF, the same command is
 * delivered again on a later uplink.  Re-executing a non-idempotent command
 * (e.g. TOGGLE) would corrupt state, so the last executed command fingerprint
 * is kept in RTC memory and a duplicate retry is ACKed WITHOUT re-execution
 * (spec A: "retry trùng lặp vẫn ACK").  REPORT (0x06) is never deduplicated.
 */
#define CMD_DEDUP_MAGIC 0xC0DEC0DE
RTC_DATA_ATTR uint32_t s_cmd_dedup_magic = 0;
RTC_DATA_ATTR uint8_t  s_cmd_dedup_cmd = 0xFF;
RTC_DATA_ATTR uint8_t  s_cmd_dedup_p1  = 0;
RTC_DATA_ATTR uint8_t  s_cmd_dedup_p2  = 0;

static void cmd_dedup_init(void)
{
    if (s_cmd_dedup_magic != CMD_DEDUP_MAGIC) {
        s_cmd_dedup_magic = CMD_DEDUP_MAGIC;
        s_cmd_dedup_cmd = 0xFF;
        s_cmd_dedup_p1 = 0;
        s_cmd_dedup_p2 = 0;
    }
}

static bool cmd_is_duplicate(uint8_t cmd, uint8_t p1, uint8_t p2)
{
    return (s_cmd_dedup_magic == CMD_DEDUP_MAGIC &&
            s_cmd_dedup_cmd == cmd &&
            s_cmd_dedup_p1 == p1 &&
            s_cmd_dedup_p2 == p2);
}

static void cmd_dedup_store(uint8_t cmd, uint8_t p1, uint8_t p2)
{
    s_cmd_dedup_magic = CMD_DEDUP_MAGIC;
    s_cmd_dedup_cmd = cmd;
    s_cmd_dedup_p1 = p1;
    s_cmd_dedup_p2 = p2;
}

/* ──────────── Frame Reception ──────────── */

/**
 * @brief Receive one complete LoRa frame (6-byte command or 8-byte packet).
 * @param buf       Output buffer (>= 8 bytes)
 * @param out_len   Received frame length (6 or 8)
 * @param timeout_ms Max time to wait for the full frame
 * @return 1 = full frame received, 0 = timeout
 */
static int lora_receive_frame(uint8_t *buf, int *out_len, int timeout_ms)
{
    int acc_len = 0;
    int elapsed = 0;

    while (elapsed < timeout_ms) {
        int n = lora_receive(&buf[acc_len], 8 - acc_len);
        if (n > 0) {
            acc_len += n;
            ESP_LOGI(TAG, "RX: +%d bytes (frame total %d)", n, acc_len);
        }
        if (acc_len >= 2) {
            int need = (buf[1] == PKT_TYPE_CMD) ? 6 : 8;
            if (acc_len >= need) {
                *out_len = need;
                return 1;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        elapsed += 10;
    }
    return 0;
}

/**
 * @brief Open the post-uplink downlink window and handle any command from the
 *        gateway (spec: gateway flushes its command cache immediately upon
 *        receiving data/heartbeat/alarm from the node).
 *
 *   - Valid command (node_id match, type 0x05, CRC8 ok) → execute + ACK 0x03,
 *     reset gatewayLostCount.
 *   - CRC mismatch or wrong node_id → NO ACK (spec A).
 *   - Legacy 8-byte ACK (type 0x03) from gateway → liveness, reset count.
 *
 * @param data Current sensor readings (used by REPORT, may be NULL)
 * @return 1 if a valid downlink was received (gateway alive), 0 otherwise
 */
static int commands_wait_downlink(const sensor_data_t *data)
{
    app_config_t *cfg = config_get();
    uint8_t buf[8];
    int len = 0;

    ESP_LOGI(TAG, "Downlink window open (%d ms)", DOWNLINK_WINDOW_MS);

    if (lora_receive_frame(buf, &len, DOWNLINK_WINDOW_MS) != 1) {
        ESP_LOGI(TAG, "Downlink window closed - no downlink received");
        return 0;
    }

    if (buf[1] == PKT_TYPE_CMD && len >= 6) {
        lora_cmd_packet_t cmd;
        memcpy(&cmd, buf, sizeof(cmd));
        uint8_t calc = crc8_xor((uint8_t *)&cmd, 5);
        ESP_LOGI(TAG, "Downlink cmd: dest=0x%02X type=0x%02X cmd=0x%02X p1=0x%02X p2=0x%02X crc=0x%02X (calc=0x%02X)",
                 cmd.dest, cmd.type, cmd.cmd, cmd.param1, cmd.param2, cmd.crc, calc);

        if (cmd.crc != calc) {
            ESP_LOGW(TAG, "Downlink CRC mismatch - NOT acknowledging (spec A)");
            lora_flush();
            return 0;
        }
        if (cmd.dest != 0xFF && cmd.dest != cfg->nodeId) {
            ESP_LOGI(TAG, "Downlink for node 0x%02X - ignoring, no ACK", cmd.dest);
            lora_flush();
            return 0;
        }
        ESP_LOGI(TAG, "Valid downlink command 0x%02X - processing", cmd.cmd);
        commands_process(&cmd, data);
        config_reset_gw_lost();
        ESP_LOGI(TAG, "gatewayLostCount reset to 0 (valid downlink)");
        return 1;
    } else if (buf[1] == PKT_TYPE_ACK && len >= 8) {
        lora_data_packet_t pkt;
        memcpy(&pkt, buf, sizeof(pkt));
        if (pkt.node_id == cfg->nodeId && crc8_verify((uint8_t *)&pkt, sizeof(pkt)) == 0) {
            ESP_LOGI(TAG, "Gateway ACK (0x03) received - gateway alive");
            config_reset_gw_lost();
            config_clear_alarm();
            return 1;
        }
        ESP_LOGW(TAG, "Gateway ACK wrong node / bad CRC - ignoring");
        lora_flush();
        return 0;
    }

    ESP_LOGD(TAG, "Unexpected downlink type 0x%02X - discarding", buf[1]);
    lora_flush();
    return 0;
}

/**
 * @brief Send an 8-byte ACK packet (type 0x03) as fast as possible.
 */
int commands_send_ack(uint8_t node_id)
{
    lora_data_packet_t ack;
    commands_build_ack_packet(node_id, &ack);

    for (int i = 0; i < ACK_SEND_RETRIES; i++) {
        if (lora_send((uint8_t *)&ack, sizeof(ack)) == sizeof(ack)) {
            ESP_LOGI(TAG, "ACK (0x03) sent to gateway");
            return 0;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGE(TAG, "Failed to send ACK (0x03)");
    return -1;
}

/**
 * @brief Send a full data packet (0x01) once, without a downlink window.
 *        Used by REPORT so command execution never recurses into listening.
 */
static int commands_send_data_simple(uint8_t node_id, const sensor_data_t *data)
{
    lora_data_packet_t pkt;
    if (commands_build_data_packet(node_id, data, &pkt) != 0) {
        return -1;
    }
    lora_flush();
    int sent = lora_send((uint8_t *)&pkt, sizeof(pkt));
    if (sent == sizeof(pkt)) {
        ESP_LOGI(TAG, "Report data packet (0x01) sent (%d bytes)", sent);
        return 0;
    }
    ESP_LOGW(TAG, "Report data send failed");
    return -1;
}

int commands_build_data_packet(uint8_t node_id, const sensor_data_t *data,
                               lora_data_packet_t *packet)
{
    if (packet == NULL || data == NULL) {
        return -1;
    }

    packet->node_id = node_id;
    packet->type = PKT_TYPE_DATA;
    
    /* Flags: bit0=pump, bit1=threshold_exceeded, bit2=gw_lost, bit3=sensor_ok */
    packet->flags = 0;
    if (pump_get_state()) {
        packet->flags |= FLAG_PUMP_STATE;
    }
    if (config_get()->thresholdExceeded) {
        packet->flags |= FLAG_THRESHOLD_EXCEEDED;
    }
    if (config_get()->gatewayLost) {
        packet->flags |= FLAG_GATEWAY_LOST;
    }
    if (data->sensor_error == 0) {
        packet->flags |= FLAG_SENSOR_OK;
    }

    packet->soil_moist = data->soil_moisture_pct;

    /* Temperature: offset +40, range -40 to +85°C */
    float temp_val = data->temperature;
    if (temp_val < -40.0f) temp_val = -40.0f;
    if (temp_val > 85.0f) temp_val = 85.0f;
    packet->temp = (uint8_t)(temp_val + 40.0f);

    /* Humidity: 0-100% */
    float hum_val = data->humidity;
    if (hum_val < 0.0f) hum_val = 0.0f;
    if (hum_val > 100.0f) hum_val = 100.0f;
    packet->humidity = (uint8_t)hum_val;

    packet->battery = data->battery;

    /* CRC8: XOR of bytes 0-6 */
    packet->crc = crc8_xor((uint8_t *)packet, 7);

    return 0;
}

int commands_build_compact_packet(uint8_t node_id, const sensor_data_t *data,
                                  uint8_t presence, uint8_t *buf)
{
    if (buf == NULL || data == NULL || presence == 0) {
        return -1;
    }

    uint8_t idx = 0;
    buf[idx++] = node_id;
    buf[idx++] = PKT_TYPE_DATA_COMPACT;
    buf[idx++] = presence;

    /* Serialise fields in LSB-first order */
    if (presence & PRESENCE_TEMP) {
        float t = data->temperature;
        if (t < -40.0f) t = -40.0f;
        if (t > 85.0f)  t = 85.0f;
        buf[idx++] = (uint8_t)(t + 40.0f);
    }
    if (presence & PRESENCE_HUM) {
        float h = data->humidity;
        if (h < 0.0f)   h = 0.0f;
        if (h > 100.0f) h = 100.0f;
        buf[idx++] = (uint8_t)h;
    }
    if (presence & PRESENCE_SOIL) {
        buf[idx++] = data->soil_moisture_pct;
    }
    if (presence & PRESENCE_BATTERY) {
        buf[idx++] = data->battery;
    }
    if (presence & PRESENCE_FLAGS) {
        uint8_t flags = 0;
        if (pump_get_state())            flags |= FLAG_PUMP_STATE;
        if (config_get()->thresholdExceeded) flags |= FLAG_THRESHOLD_EXCEEDED;
        if (config_get()->gatewayLost)   flags |= FLAG_GATEWAY_LOST;
        if (data->sensor_error == 0)     flags |= FLAG_SENSOR_OK;
        buf[idx++] = flags;
    }

    /* CRC8: XOR of all bytes before CRC */
    buf[idx] = crc8_xor(buf, idx);

    return idx + 1;  /* payload length + CRC byte */
}

int commands_send_compact_with_ack(uint8_t node_id, const sensor_data_t *data,
                                    uint8_t presence)
{
    uint8_t pkt[8];                     /* max compact = 8 bytes */
    int retries = UPLINK_RETRIES;
    bool sent = false;

    int pkt_len = commands_build_compact_packet(node_id, data, presence, pkt);
    if (pkt_len < 0) {
        return -1;
    }

    while (retries--) {
        ESP_LOGI(TAG, "Sending compact packet (0x06, %d bytes, presence=0x%02X) attempt %d/%d",
                 pkt_len, presence, UPLINK_RETRIES - retries, UPLINK_RETRIES);

        lora_flush();
        if (lora_send(pkt, pkt_len) != pkt_len) {
            ESP_LOGW(TAG, "LoRa compact send failed");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        sent = true;

        /* Spec B: after uplink, wait for downlink (200-500ms) and ACK any
         * pending command.  The gateway flushes its cache on data receipt. */
        if (commands_wait_downlink(data) == 1) {
            return 0;
        }

        ESP_LOGW(TAG, "No downlink after compact send (attempt %d)", UPLINK_RETRIES - retries);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (!sent) {
        ESP_LOGE(TAG, "All compact send attempts failed (transport)");
        return -1;
    }

    /* Transmitted but no downlink response this cycle.  Lost detection is
     * driven by HEARTBEAT (spec C), so no counter bump here. */
    ESP_LOGI(TAG, "Compact packet transmitted (no downlink response)");
    return 0;
}

int commands_build_ack_packet(uint8_t node_id, lora_data_packet_t *packet)
{
    if (packet == NULL) {
        return -1;
    }

    memset(packet, 0, sizeof(lora_data_packet_t));
    packet->node_id = node_id;
    packet->type = PKT_TYPE_ACK;
    
    packet->flags = 0;
    if (pump_get_state()) {
        packet->flags |= FLAG_PUMP_STATE;
    }
    if (config_get()->gatewayLost) {
        packet->flags |= FLAG_GATEWAY_LOST;
    }
    packet->battery = 0xFF; /* External power */

    packet->crc = crc8_xor((uint8_t *)packet, 7);

    return 0;
}

int commands_build_alarm_packet(uint8_t node_id, uint8_t alarm_code, lora_data_packet_t *packet)
{
    if (packet == NULL) {
        return -1;
    }

    memset(packet, 0, sizeof(lora_data_packet_t));
    packet->node_id = node_id;
    packet->type = PKT_TYPE_ALARM;
    packet->flags = alarm_code;       /* Place alarm code in flags byte */
    packet->soil_moist = alarm_code;   /* Place alarm code in soil_moist byte for redundancy */
    packet->battery = 0xFF;

    packet->crc = crc8_xor((uint8_t *)packet, 7);

    return 0;
}

int commands_process(const lora_cmd_packet_t *packet, const sensor_data_t *data)
{
    if (packet == NULL) {
        return -1;
    }

    cmd_dedup_init();

    app_config_t *cfg = config_get();

    /* 0x09 = gateway keep-alive / empty ACK ("no pending command").  The
     * gateway sends it after every uplink when it has nothing to send.
     * Per spec the node does NOT execute any command and does NOT ACK back —
     * it only treats the gateway as alive (resets gatewayLostCount). */
    if (packet->cmd == CMD_SYNC_TIME) {
        ESP_LOGI(TAG, "0x09 keep-alive (no command) - gateway alive, resetting gatewayLostCount (no ACK)");
        config_reset_gw_lost();
        return 0;
    }

    ESP_LOGI(TAG, "Executing command 0x%02X (p1=0x%02X p2=0x%02X)",
             packet->cmd, packet->param1, packet->param2);

    /* Idempotency (spec A): a duplicate gateway retry is ACKed but NOT
     * re-executed, keeping non-idempotent commands (TOGGLE) safe.
     * REPORT (0x06) always executes - it is the gateway's heartbeat ACK. */
    if (packet->cmd != CMD_REQUEST_REPORT &&
        cmd_is_duplicate(packet->cmd, packet->param1, packet->param2)) {
        ESP_LOGI(TAG, "Duplicate command 0x%02X (gateway retry) - ACKing without re-execution",
                 packet->cmd);
        commands_send_ack(cfg->nodeId);
        return 0;
    }

    switch (packet->cmd) {
        case CMD_SET_INTERVAL: {
            uint16_t interval = packet->param1 | (packet->param2 << 8);
            if (interval < 5) interval = 5;
            config_set_sleep_time(interval);
            ESP_LOGI(TAG, "Cmd: Set sleep interval = %u s", interval);
            break;
        }

        case CMD_RELAY_ON: {
            uint16_t duration = packet->param1 | (packet->param2 << 8);
            pump_on();
            if (duration > 0) {
                cfg->pumpBySchedule = true;
                cfg->scheduleDuration = duration;
                config_save();
                ESP_LOGI(TAG, "Cmd: Relay ON for %u seconds", duration);
            } else {
                cfg->pumpBySchedule = false;
                config_save();
                ESP_LOGI(TAG, "Cmd: Relay ON (infinite)");
            }
            break;
        }

        case CMD_RELAY_OFF: {
            pump_off();
            cfg->pumpBySchedule = false;
            config_save();
            ESP_LOGI(TAG, "Cmd: Relay OFF");
            break;
        }

        case CMD_SET_THRESHOLDS: {
            config_set_thresholds(packet->param1, packet->param2);
            ESP_LOGI(TAG, "Cmd: Set thresholds Low=%d%% High=%d%%", packet->param1, packet->param2);
            break;
        }

        case CMD_SET_SCHEDULE: {
            config_set_schedule(packet->param1, packet->param2);
            ESP_LOGI(TAG, "Cmd: Set schedule at %02d:%02d", packet->param1, packet->param2);
            break;
        }

        case CMD_REQUEST_REPORT: {
            /* Spec: gateway sends REPORT (0x06) as heartbeat ACK → reset count */
            ESP_LOGI(TAG, "Cmd: REPORT - gateway alive, resetting gatewayLostCount");
            config_reset_gw_lost();

            sensor_data_t d;
            if (data != NULL) {
                d = *data;
            } else {
                power_sensor_on();
                vTaskDelay(pdMS_TO_TICKS(100));
                sensors_read(&d);
                power_sensor_off();
            }

            ESP_LOGI(TAG, "Cmd: REPORT - sending sensor data response");
            commands_send_data_simple(cfg->nodeId, &d);
            break;
        }

        case CMD_TOGGLE_PUMP: {
            pump_toggle();
            ESP_LOGI(TAG, "Cmd: Toggle pump -> %s", pump_get_state() ? "ON" : "OFF");
            break;
        }

        case CMD_SET_MODE: {
            uint8_t mode = packet->param1;
            if (mode <= MODE_THRESHOLD) {
                config_set_mode((operation_mode_t)mode);
                ESP_LOGI(TAG, "Cmd: Set mode = %d", mode);
            } else {
                ESP_LOGW(TAG, "Invalid mode requested: %d", mode);
            }
            break;
        }

        /* CMD_SYNC_TIME (0x09) is handled at the top of commands_process as a
         * keep-alive no-op — it never reaches the switch. */

        case CMD_SET_DELTA_THRESHOLDS: {
            uint8_t dtype = packet->param1;
            uint8_t dval  = packet->param2;
            switch (dtype) {
                case DELTA_TYPE_TEMPERATURE:
                    config_set_delta_temp(dval);
                    break;
                case DELTA_TYPE_HUMIDITY:
                    config_set_delta_humidity(dval);
                    break;
                case DELTA_TYPE_SOIL:
                    config_set_delta_soil(dval);
                    break;
                case DELTA_TYPE_BATTERY:
                    config_set_delta_battery(dval);
                    break;
                default:
                    ESP_LOGW(TAG, "Unknown delta threshold type: %d", dtype);
                    break;
            }
            save_threshold_to_nvs();
            ESP_LOGI(TAG, "Cmd: Set delta threshold type=%d value=%d", dtype, dval);
            break;
        }

        default:
            /* Spec A: a valid downlink is ACKed even if the command is unknown */
            ESP_LOGW(TAG, "Unknown command: 0x%02X - ACKing valid downlink", packet->cmd);
            break;
    }

    /* Remember fingerprint for idempotent duplicate handling */
    cmd_dedup_store(packet->cmd, packet->param1, packet->param2);

    /* Spec E: send ACK 0x03 AFTER executing the command */
    commands_send_ack(cfg->nodeId);

    return 0;
}

int commands_send_data_with_ack(uint8_t node_id, const sensor_data_t *sensor_data)
{
    lora_data_packet_t data_pkt;
    int retries = UPLINK_RETRIES;
    bool sent = false;

    if (commands_build_data_packet(node_id, sensor_data, &data_pkt) != 0) {
        return -1;
    }

    while (retries--) {
        ESP_LOGI(TAG, "Sending data packet (0x01, attempt %d/%d)",
                 UPLINK_RETRIES - retries, UPLINK_RETRIES);

        lora_flush();
        if (lora_send((uint8_t *)&data_pkt, sizeof(data_pkt)) != sizeof(data_pkt)) {
            ESP_LOGW(TAG, "LoRa send failed");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        sent = true;

        /* Spec B: after uplink, wait for downlink (200-500ms) and ACK any
         * pending command.  The gateway flushes its cache on data receipt. */
        if (commands_wait_downlink(sensor_data) == 1) {
            return 0;
        }

        ESP_LOGW(TAG, "No downlink after data send (attempt %d)", UPLINK_RETRIES - retries);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (!sent) {
        ESP_LOGE(TAG, "All data send attempts failed (transport)");
        return -1;
    }

    /* Transmitted but no downlink response this cycle.  Lost detection is
     * driven by HEARTBEAT (spec C), so no counter bump here. */
    ESP_LOGI(TAG, "Data packet transmitted (no downlink response)");
    return 0;
}

int commands_build_heartbeat_packet(uint8_t node_id, uint8_t battery,
                                    const sensor_data_t *data, lora_data_packet_t *packet)
{
    if (packet == NULL) return -1;

    memset(packet, 0, sizeof(lora_data_packet_t));
    packet->node_id = node_id;
    packet->type = PKT_TYPE_HEARTBEAT;

    /* Flags: bit0=pump, bit1=threshold_exceeded, bit2=gw_lost, bit3=sensor_ok */
    packet->flags = 0;
    if (pump_get_state())                packet->flags |= FLAG_PUMP_STATE;
    if (config_get()->thresholdExceeded) packet->flags |= FLAG_THRESHOLD_EXCEEDED;
    if (config_get()->gatewayLost)       packet->flags |= FLAG_GATEWAY_LOST;
    if (data != NULL && data->sensor_error == 0) packet->flags |= FLAG_SENSOR_OK;

    /* Include current readings (8-byte heartbeat format) */
    if (data != NULL) {
        float t = data->temperature;
        if (t < -40.0f) t = -40.0f;
        if (t > 85.0f)  t = 85.0f;
        packet->temp = (uint8_t)(t + 40.0f);

        float h = data->humidity;
        if (h < 0.0f)   h = 0.0f;
        if (h > 100.0f) h = 100.0f;
        packet->humidity = (uint8_t)h;

        packet->soil_moist = data->soil_moisture_pct;
    }

    packet->battery = battery;
    packet->crc = crc8_xor((uint8_t *)packet, 7);

    return 0;
}

int commands_send_heartbeat(uint8_t node_id, uint8_t battery, const sensor_data_t *data)
{
    lora_data_packet_t hb_pkt;
    int retries = UPLINK_RETRIES;
    bool sent = false;

    if (commands_build_heartbeat_packet(node_id, battery, data, &hb_pkt) != 0) {
        return -1;
    }

    while (retries--) {
        ESP_LOGI(TAG, "Sending heartbeat (0x02, attempt %d/%d)",
                 UPLINK_RETRIES - retries, UPLINK_RETRIES);

        lora_flush();
        if (lora_send((uint8_t *)&hb_pkt, sizeof(hb_pkt)) != sizeof(hb_pkt)) {
            ESP_LOGW(TAG, "LoRa heartbeat send failed");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        sent = true;

        /* Spec C: after heartbeat, wait for downlink and ACK.  The gateway
         * answers heartbeats with REPORT (0x06) or flushes pending commands.
         * ANY valid downlink resets gatewayLostCount. */
        if (commands_wait_downlink(data) == 1) {
            return 0;
        }

        ESP_LOGW(TAG, "No downlink after heartbeat (attempt %d)", UPLINK_RETRIES - retries);
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    if (!sent) {
        ESP_LOGE(TAG, "All heartbeat attempts failed (transport)");
        return -1;
    }

    /* Spec C: no downlink within gatewayLostTimeout (e.g. %d s) → count++.
     * At >= %d → GW_LOST flag (bit2) + alarm 0x05 (sent by main loop). */
    ESP_LOGW(TAG, "Heartbeat sent but no downlink received - gatewayLostCount++");
    config_increment_gw_lost();

    if (config_get()->gatewayLostCount == GW_LOST_ALARM_THRESHOLD) {
        ESP_LOGE(TAG, "gatewayLostCount >= %d - setting GW_LOST flag + alarm 0x05",
                 GW_LOST_ALARM_THRESHOLD);
        config_set_alarm(ALARM_GATEWAY_LOST);
    }
    if (config_get()->gatewayLost) {
        irrigation_gateway_lost();
    }

    return 0;
}

int commands_send_alarm(uint8_t node_id, uint8_t alarm_code, const sensor_data_t *data)
{
    lora_data_packet_t alarm_pkt;
    if (commands_build_alarm_packet(node_id, alarm_code, &alarm_pkt) != 0) {
        return -1;
    }

    ESP_LOGI(TAG, "Sending ALARM packet (0x04, code: 0x%02X)", alarm_code);
    lora_flush();
    if (lora_send((uint8_t *)&alarm_pkt, sizeof(alarm_pkt)) != sizeof(alarm_pkt)) {
        ESP_LOGE(TAG, "Alarm send failed");
        return -1;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    /* Spec D: after alarm, wait for downlink and ACK any pending command */
    commands_wait_downlink(data);
    return 0;
}

int commands_check_pending(const sensor_data_t *data)
{
    app_config_t *cfg = config_get();
    uint8_t buf[8];
    int len = 0;

    /* Give the module a moment to push any buffered frame into UART */
    vTaskDelay(pdMS_TO_TICKS(20));

    if (lora_receive_frame(buf, &len, PENDING_WINDOW_MS) != 1) {
        return 1;   /* No packet pending */
    }

    if (buf[1] != PKT_TYPE_CMD) {
        ESP_LOGD(TAG, "Non-command packet type 0x%02X - discarding", buf[1]);
        lora_flush();
        return 1;
    }

    lora_cmd_packet_t cmd_pkt;
    memcpy(&cmd_pkt, buf, 6);

    uint8_t expected_crc = crc8_xor((uint8_t *)&cmd_pkt, 5);
    if (cmd_pkt.crc != expected_crc) {
        ESP_LOGW(TAG, "Command CRC mismatch: got 0x%02X, expected 0x%02X - NO ACK",
                 cmd_pkt.crc, expected_crc);
        lora_flush();
        return -1;
    }

    if (cmd_pkt.dest != 0xFF && cmd_pkt.dest != cfg->nodeId) {
        ESP_LOGI(TAG, "Command for another node (0x%02X) - ignoring", cmd_pkt.dest);
        lora_flush();
        return 1;
    }

    ESP_LOGI(TAG, "Pending command 0x%02X - processing", cmd_pkt.cmd);
    commands_process(&cmd_pkt, data);
    config_reset_gw_lost();
    ESP_LOGI(TAG, "gatewayLostCount reset to 0 (valid downlink)");
    return 0;
}