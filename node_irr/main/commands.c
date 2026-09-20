#include "commands.h"
#include "lora_uart.h"
#include "config.h"
#include "sensors.h"
#include "pump.h"
#include "power.h"
#include "irrigation.h"
#include "baseline.h"
#include "crc.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <string.h>

static const char *TAG = "COMMANDS";

/* ──────────── Gateway Protocol Timing ──────────── */
#define UPLINK_RETRIES        3   /* Max uplink transmit attempts */
#define ACK_SEND_RETRIES      3   /* ACK transmit retries (target < 500 ms) */
#define PENDING_WINDOW_MS     250 /* Listen window when no uplink was just sent */

/* Downlink burst draining.
 *
 * A baseline update arrives as SEVERAL 0x07 chunks back-to-back (the gateway
 * sends BASELINE_CHUNKS_PER_FLUSH chunks per opportunity), so a downlink window
 * must consume the whole burst. Each lora_receive_frame() call reads at most one
 * frame, therefore the window is drained frame-by-frame until the line goes
 * idle — reading a single frame would lose the rest of the burst and the series
 * could never complete. */
#define DOWNLINK_IDLE_GAP_MS   150  /* idle time that closes the window (no burst) */
#define DOWNLINK_BURST_GAP_MS  400  /* max gap between two baseline chunks */
#define DOWNLINK_BURST_MAX_MS  3000 /* hard cap on one drain (awake-time guard) */

/* Boot-sync baseline window.
 *
 * LoRa là BÁN SONG CÔNG: khi node phát (DONE 0x08) thì nó KHÔNG thu được, và
 * gateway cũng không nghe được gì (đang phát burst). Vì vậy node phải CHỜ HẾT
 * burst chunk của gateway (các chunk cách nhau ~300 ms) rồi mới commit + phát
 * 0x08, nếu không: node mất chunk serie 1/2, gateway mất DONE → gateway re-arm
 * và gửi lại từ serie 0 mãi mãi (chunk storm, WDT reset gateway). */
#define BASELINE_BURST_IDLE_MS 800
#define BASELINE_DONE_GAP_MS   400  /* giãn cách giữa các DONE: phải LỚN HƠN
                                     * cửa sổ TX ACK của gateway (~265 ms) để
                                     * gateway đọc được từng frame một. */

/* Retry backoff between uplink attempts when no downlink was received. The
 * delay grows with each attempt (base << attempt) so the node does not spam
 * the air: e.g. 1000ms -> 2000ms -> 4000ms, capped at 8s. */
#define RETRY_DELAY_BASE_MS   1000
#define RETRY_DELAY_MAX_MS    8000

static int retry_delay_ms(int attempt_no) /* 0-based */
{
    uint32_t d = (uint32_t)RETRY_DELAY_BASE_MS << attempt_no;
    if (d > RETRY_DELAY_MAX_MS) d = RETRY_DELAY_MAX_MS;
    return (int)d;
}

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
/**
 * @brief Return the total frame length implied by the frame header, or 0 if
 *        more header bytes are still needed.
 *
 * Supported downlink frames:
 *   - 0x05 command : fixed 7 bytes (dest|type|cmd|p1|p2|slot|crc)
 *   - 0x07 baseline: 7 + 2*n_points + 1 (variable)
 *   - anything else: legacy 8-byte frame (ACK etc.)
 */
static int lora_frame_need(const uint8_t *buf, int acc_len)
{
    if (acc_len < 2) return 0;

    uint8_t type = buf[1];
    if (type == PKT_TYPE_CMD) {
        return (int)sizeof(lora_cmd_packet_t);      /* 7 */
    }
    if (type == PKT_TYPE_BASELINE) {
        if (acc_len < 7) return 0;                  /* need n_points first */
        int n = buf[6];
        if (n > BASELINE_POINTS_PER_CHUNK) n = BASELINE_POINTS_PER_CHUNK;
        return 7 + 2 * n + 1;
    }
    return 8;                                       /* legacy / ACK */
}

/**
 * @brief Receive one complete LoRa frame (command, baseline chunk or packet).
 * @param buf        Output buffer (must be >= BASELINE_MAX_FRAME bytes)
 * @param buf_size   Capacity of buf
 * @param out_len    Received frame length
 * @param timeout_ms Max time to wait for the full frame
 * @return 1 = full frame received, 0 = timeout
 */
/* ──────────── RX staging (frame reassembly across UART reads) ────────────
 *
 * lora_receive() hands back whatever the UART happens to hold at that moment,
 * and two baseline chunks (14 B each) very often arrive inside ONE read. The
 * old code read straight into a single-frame buffer, so any frame behind the
 * first one was silently DROPPED (and the buffer-size warning fired on every
 * burst). Stage the raw bytes here and hand out exactly one frame per call;
 * leftover bytes stay for the next call.
 */
#define RX_STAGE_SIZE   192
#define RX_RESYNC_MS    300   /* no progress for this long → drop 1 byte */

static uint8_t s_rx_stage[RX_STAGE_SIZE];
static int     s_rx_len = 0;

/**
 * @brief Receive one complete LoRa frame (command, baseline chunk or packet).
 * @param buf        Output buffer (must be >= BASELINE_MAX_FRAME bytes)
 * @param buf_size   Capacity of buf
 * @param out_len    Received frame length
 * @param timeout_ms Max time to wait for the full frame
 * @return 1 = full frame received, 0 = timeout
 */
static int lora_receive_frame(uint8_t *buf, int buf_size, int *out_len, int timeout_ms)
{
    int64_t start_us         = esp_timer_get_time();
    int64_t last_progress_us = start_us;

    while (1) {
        /* 1) Hand out a complete frame already held in the stage. */
        if (s_rx_len > 0) {
            int need = lora_frame_need(s_rx_stage, s_rx_len);

            if (need > 0 && s_rx_len >= need) {
                if (need > buf_size) need = buf_size;   /* paranoia */
                memcpy(buf, s_rx_stage, (size_t)need);
                memmove(&s_rx_stage[0], &s_rx_stage[need],
                        (size_t)(s_rx_len - need));
                s_rx_len -= need;
                *out_len = need;
                return 1;
            }

            /* A frame longer than the stage can ever hold is unrecoverable. */
            if (need > RX_STAGE_SIZE) {
                ESP_LOGW(TAG, "Frame len %d > stage %d - dropping staged bytes",
                         need, RX_STAGE_SIZE);
                s_rx_len = 0;
            }
        }

        if ((esp_timer_get_time() - start_us) / 1000 >= timeout_ms) {
            return 0;
        }

        /* 2) Refill the stage from the UART. */
        if (s_rx_len < RX_STAGE_SIZE) {
            int n = lora_receive(&s_rx_stage[s_rx_len],
                                 (size_t)(RX_STAGE_SIZE - s_rx_len));
            if (n > 0) {
                s_rx_len += n;
                last_progress_us = esp_timer_get_time();
                ESP_LOGI(TAG, "RX: +%d bytes (staged %d)", n, s_rx_len);
                continue;               /* try to frame immediately */
            }
        }

        /* 3) Resync: bytes that never complete a frame are noise (a truncated
         *    tail, a corrupted chunk, ...). Drop the oldest byte so a good
         *    frame behind them is not blocked forever. A stage holding MORE
         *    than one maximum frame that still cannot be framed is definitely
         *    garbage (valid frames always frame immediately), so flush it whole
         *    instead of nibbling 1 byte every RX_RESYNC_MS. */
        if (s_rx_len > 0 &&
            (esp_timer_get_time() - last_progress_us) / 1000 >= RX_RESYNC_MS) {
            if (s_rx_len > BASELINE_MAX_FRAME) {
                ESP_LOGW(TAG, "RX resync: flushing %d garbage byte(s)", s_rx_len);
                s_rx_len = 0;
            } else {
                ESP_LOGW(TAG, "RX resync: dropping 1 stale byte (staged %d)", s_rx_len);
                memmove(&s_rx_stage[0], &s_rx_stage[1], (size_t)(s_rx_len - 1));
                s_rx_len--;
            }
            last_progress_us = esp_timer_get_time();
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * @brief Handle ONE downlink frame (already framed and length-validated).
 *
 * @return 1 = valid downlink (gateway alive), 0 = frame consumed but ignored,
 *        -1 = consumed with a CRC / destination error (no ACK, spec A)
 */
static int process_downlink_frame(const uint8_t *buf, int len,
                                  const sensor_data_t *data)
{
    app_config_t *cfg = config_get();

    /* LoRa is a shared channel: every node hears every frame. Anything not
     * addressed to us (or broadcast) must be ignored WITHOUT touching state —
     * most importantly a 0x07 chunk addressed to another node must never be
     * stored as ours. */
    if (buf[0] != 0xFF && buf[0] != cfg->nodeId) {
        ESP_LOGD(TAG, "Frame for node 0x%02X - ignoring (we are 0x%02X)",
                 buf[0], cfg->nodeId);
        return 0;
    }

    if (buf[1] == PKT_TYPE_BASELINE) {
        /* Baseline chunk (type 0x07) — reassembled by baseline.c. A chunk is a
         * valid downlink, so the gateway is alive: keep the lost-counter down. */
        baseline_handle_chunk(buf, len);
        config_reset_gw_lost();
        return 1;
    }

    if (buf[1] == PKT_TYPE_CMD && len >= (int)sizeof(lora_cmd_packet_t)) {
        lora_cmd_packet_t cmd;
        memcpy(&cmd, buf, sizeof(cmd));
        uint8_t calc = crc8_xor((uint8_t *)&cmd, sizeof(cmd) - 1);
        ESP_LOGI(TAG, "Downlink cmd: dest=0x%02X type=0x%02X cmd=0x%02X p1=0x%02X p2=0x%02X slot=%u crc=0x%02X (calc=0x%02X)",
                 cmd.dest, cmd.type, cmd.cmd, cmd.param1, cmd.param2, cmd.slot, cmd.crc, calc);

        if (cmd.crc != calc) {
            ESP_LOGW(TAG, "Downlink CRC mismatch - NOT acknowledging (spec A)");
            lora_flush();
            return -1;
        }
        if (cmd.dest != 0xFF && cmd.dest != cfg->nodeId) {
            ESP_LOGI(TAG, "Downlink for node 0x%02X - ignoring, no ACK", cmd.dest);
            lora_flush();
            return -1;
        }
        /* Every valid downlink carries the gateway's current slot → time sync. */
        config_set_current_slot(cmd.slot);

        ESP_LOGI(TAG, "Valid downlink command 0x%02X - processing", cmd.cmd);
        commands_process(&cmd, data);
        config_reset_gw_lost();
        /* Gateway is back — the GATEWAY_LOST alarm (0x05) is resolved. Clear
         * it so the node sends the "alarm cleared" packet on the next cycle. */
        if (config_get()->alarmCode == ALARM_GATEWAY_LOST) {
            config_clear_alarm();
        }
        ESP_LOGI(TAG, "gatewayLostCount reset to 0 (valid downlink)");
        return 1;
    }

    if (buf[1] == PKT_TYPE_ACK && len >= 8) {
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
        return -1;
    }

    ESP_LOGD(TAG, "Unexpected downlink type 0x%02X - discarding", buf[1]);
    lora_flush();
    return 0;
}

/**
 * @brief Open the post-uplink downlink window and handle EVERYTHING the gateway
 *        sends (spec: it flushes its command cache — and any pending baseline
 *        chunks — immediately after receiving data/heartbeat/alarm).
 *
 * The window is drained until the line goes idle, using short idle gaps so a
 * plain ACK still closes it quickly (power).
 *
 * @param data Current sensor readings (used by REPORT, may be NULL)
 * @return 1 if a valid downlink was received (gateway alive), 0 otherwise
 */
static int commands_wait_downlink(const sensor_data_t *data)
{
    uint8_t buf[BASELINE_MAX_FRAME];
    int len = 0;
    int result = 0;

    ESP_LOGI(TAG, "Downlink window open (%d ms)", DOWNLINK_WINDOW_MS);

    if (lora_receive_frame(buf, sizeof(buf), &len, DOWNLINK_WINDOW_MS) != 1) {
        ESP_LOGI(TAG, "Downlink window closed - no downlink received");
        return 0;
    }

    int r = process_downlink_frame(buf, len, data);
    if (r == 1) result = 1;

    /* Drain the rest of the burst: keep reading while frames keep arriving. */
    uint32_t gap_ms = (buf[1] == PKT_TYPE_BASELINE) ? DOWNLINK_BURST_GAP_MS
                                                    : DOWNLINK_IDLE_GAP_MS;
    uint64_t start_us = (uint64_t)esp_timer_get_time();

    while ((((uint64_t)esp_timer_get_time() - start_us) / 1000) < DOWNLINK_BURST_MAX_MS) {
        if (lora_receive_frame(buf, sizeof(buf), &len, (int)gap_ms) != 1) {
            break;                                  /* line idle → burst over */
        }
        if (process_downlink_frame(buf, len, data) == 1) result = 1;
        gap_ms = (buf[1] == PKT_TYPE_BASELINE) ? DOWNLINK_BURST_GAP_MS
                                               : DOWNLINK_IDLE_GAP_MS;
    }

    if (result == 0) {
        ESP_LOGI(TAG, "Downlink window closed - nothing usable");
    }
    return result;
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
        vTaskDelay(pdMS_TO_TICKS(retry_delay_ms(UPLINK_RETRIES - retries - 1)));
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
            /* Runtime-configurable deep-sleep period (initial default 300 s).
             * config_set_sleep_time() clamps to [5, 3600] s and keeps the value
             * in RTC config, so it survives deep-sleep cycles. The gateway
             * applies the same change to its own node entry
             * (node_set_sleep_interval) and derives the per-node offline
             * timeout from it, so both sides stay in sync. */
            uint16_t requested = packet->param1 | (packet->param2 << 8);
            config_set_sleep_time(requested);
            break;
        }

        case CMD_RELAY_ON: {
            uint16_t duration = packet->param1 | (packet->param2 << 8);
            pump_on();
            if (duration > 0) {
                /* Timed run: the deadline lives on the RTC clock, so the pump
                 * keeps running for the full duration even if the node wakes up
                 * early for another reason (park, button, gateway polling). */
                irrigation_start_timed_run(duration);
                ESP_LOGI(TAG, "Cmd: Relay ON for %u seconds", duration);
            } else {
                /* Infinite ON = manual hold, no automatic switch-off. */
                irrigation_cancel_timed_run();
                ESP_LOGI(TAG, "Cmd: Relay ON (infinite)");
            }
            break;
        }

        case CMD_RELAY_OFF: {
            pump_off();
            irrigation_cancel_timed_run();
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
            /* A manual toggle is an explicit override: end any timed run so its
             * deadline cannot switch the pump off later on. */
            irrigation_cancel_timed_run();
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
        vTaskDelay(pdMS_TO_TICKS(retry_delay_ms(UPLINK_RETRIES - retries - 1)));
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

    /* Heartbeat carries only flags + battery (spec: soil/temp/hum = 0) —
     * it is a liveness signal, sensor data goes out via data packets. */
    packet->soil_moist = 0;
    packet->temp = 0;
    packet->humidity = 0;

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
        vTaskDelay(pdMS_TO_TICKS(retry_delay_ms(UPLINK_RETRIES - retries - 1)));
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

    /* Alarm packets are fire-and-forget, but we retry a couple of times to make
     * sure the gateway actually received the event (reliability vs. RF loss).
     * We stop as soon as we get any downlink ACK from the gateway. */
    const int ALARM_SEND_RETRIES = 3;

    for (int i = 0; i < ALARM_SEND_RETRIES; i++) {
        ESP_LOGI(TAG, "Sending ALARM packet (0x04, code: 0x%02X) attempt %d/%d",
                 alarm_code, i + 1, ALARM_SEND_RETRIES);

        lora_flush();
        if (lora_send((uint8_t *)&alarm_pkt, sizeof(alarm_pkt)) != sizeof(alarm_pkt)) {
            ESP_LOGW(TAG, "Alarm send failed (attempt %d)", i + 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Spec D: after alarm, wait for downlink and ACK any pending command.
         * A received downlink also proves the gateway is alive/reachable. */
        if (commands_wait_downlink(data) == 1) {
            return 0;
        }

        vTaskDelay(pdMS_TO_TICKS(retry_delay_ms(i)));
    }

    ESP_LOGW(TAG, "Alarm 0x%02X sent %d times without downlink ACK", alarm_code,
             ALARM_SEND_RETRIES);
    return 0;   /* Not fatal — alarm is fire-and-forget */
}

int commands_check_pending(const sensor_data_t *data)
{
    uint8_t buf[BASELINE_MAX_FRAME];
    int len = 0;
    int rc  = 1;                    /* 1 = nothing consumed */

    /* Give the module a moment to push any buffered frame into UART */
    vTaskDelay(pdMS_TO_TICKS(20));

    if (lora_receive_frame(buf, sizeof(buf), &len, PENDING_WINDOW_MS) != 1) {
        return 1;   /* No packet pending */
    }

    /* Same frame handling as the post-uplink window (shared helper). */
    int r = process_downlink_frame(buf, len, data);
    if (r == 1)      rc = 0;
    else if (r < 0)  rc = -1;

    /* Drain the rest of a burst (baseline push / several cached commands). */
    uint32_t gap_ms = (buf[1] == PKT_TYPE_BASELINE) ? DOWNLINK_BURST_GAP_MS
                                                    : DOWNLINK_IDLE_GAP_MS;
    uint64_t start_us = (uint64_t)esp_timer_get_time();

    while ((((uint64_t)esp_timer_get_time() - start_us) / 1000) < DOWNLINK_BURST_MAX_MS) {
        if (lora_receive_frame(buf, sizeof(buf), &len, (int)gap_ms) != 1) {
            break;                                  /* line idle → burst over */
        }
        if (process_downlink_frame(buf, len, data) == 1) {
            rc = 0;
        } else if (rc != 0) {
            rc = -1;
        }
        gap_ms = (buf[1] == PKT_TYPE_BASELINE) ? DOWNLINK_BURST_GAP_MS
                                               : DOWNLINK_IDLE_GAP_MS;
    }
    return rc;
}

/* ──────────── Boot sync: baseline / time request ──────────── */

int commands_send_req(uint8_t flags, uint8_t series_mask)
{
    app_config_t *cfg = config_get();
    uint8_t buf[8];

    buf[0] = cfg->nodeId;
    buf[1] = PKT_TYPE_REQ;        /* 0x09 */
    buf[2] = flags;
    buf[3] = series_mask & 0x07u;
    buf[4] = 0;
    buf[5] = 0;
    buf[6] = 0;
    buf[7] = crc8_xor(buf, 7);

    for (int i = 0; i < ACK_SEND_RETRIES; i++) {
        if (lora_send(buf, sizeof(buf)) == sizeof(buf)) {
            ESP_LOGI(TAG, "REQ sent (flags=0x%02X mask=0x%02X)", flags, series_mask);
            return 0;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ESP_LOGW(TAG, "REQ transport failed (flags=0x%02X)", flags);
    return -1;
}

/**
 * @brief Commit a fully-received baseline session and tell the gateway.
 *
 * Exposed so BOTH the boot-sync listen window and the normal downlink window
 * can finish the transfer the instant the last chunk lands. Waiting for the
 * whole listen window left the gateway believing the transfer was still
 * pending, so it re-flushed every chunk on each opportunity (chunk storm).
 *
 * @return bitmask of series newly committed (0 = nothing to report yet)
 */
int commands_finish_baseline_if_ready(void)
{
    if (!baseline_session_active() || !baseline_session_complete()) return 0;

    uint8_t done = baseline_commit();
    if (done == 0) return 0;

    /* Báo lại TẤT CẢ serie node đang có trong NVS chứ không chỉ serie vừa
     * commit: 0x08 là idempotent, nên nếu lần trước gateway mất 1 frame DONE
     * trên không thì lần này nó được gửi lại — tránh việc gateway phải giữ lượt
     * của node này trong khi node đã có đủ dữ liệu. */
    uint8_t report = (uint8_t)(baseline_valid_mask() & 0x07u);
    ESP_LOGI(TAG, "Baseline stored (mask=0x%02X) at slot t=%u - reporting 0x%02X to gateway",
             done, config_get_slot(), report);

    /* Gộp các serie CÙNG version vào 1 frame bitmask → bình thường (lần cấp đầu,
     * cả 3 serie đều v1) chỉ phát ĐÚNG 1 frame 0x08 thay vì 3 frame liền nhau. */
    uint8_t left = report;
    int     sent = 0;
    while (left != 0) {
        uint8_t s = 0;
        while (s < BASELINE_SERIES_COUNT && !(left & (uint8_t)(1u << s))) s++;
        if (s >= BASELINE_SERIES_COUNT) break;

        uint8_t v     = baseline_version(s);
        uint8_t group = 0;
        for (uint8_t k = 0; k < BASELINE_SERIES_COUNT; k++) {
            if ((left & (uint8_t)(1u << k)) && baseline_version(k) == v) {
                group |= (uint8_t)(1u << k);
            }
        }

        if (sent > 0) vTaskDelay(pdMS_TO_TICKS(BASELINE_DONE_GAP_MS));
        commands_send_baseline_done_mask(group, v);
        left &= (uint8_t)~group;
        sent++;
    }
    return (int)done;
}

void commands_wait_baseline(uint32_t timeout_ms)
{
    app_config_t *cfg = config_get();
    uint8_t buf[BASELINE_MAX_FRAME];
    int  len = 0;
    uint32_t start      = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t last_rx_ms = start;
    uint32_t last_ack_ms = 0;
    bool     got_chunk  = false;

    while ((uint32_t)(esp_timer_get_time() / 1000) - start < timeout_ms) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        if (lora_receive_frame(buf, sizeof(buf), &len, 500) != 1) {
            /* Line im trong slice này: đủ slot + đủ 3 serie ⇒ xong. */
            if (config_slot_valid() && baseline_valid_mask() == 0x07u) break;

            /* Line im ≥ BASELINE_BURST_IDLE_MS kể từ chunk cuối ⇒ burst của
             * gateway đã phát xong. CHỈ BÂY GIỜ mới commit + phát 0x08 (xem lý
             * do bán song công ở BASELINE_BURST_IDLE_MS). Nhờ vậy gateway nghe
             * được DONE và không còn phải flush lại từ serie 0. */
            if (got_chunk &&
                (uint32_t)(now_ms - last_rx_ms) >= BASELINE_BURST_IDLE_MS) {
                if (commands_finish_baseline_if_ready() != 0) {
                    got_chunk   = false;
                    last_ack_ms = now_ms;
                }
                if (config_slot_valid() && baseline_valid_mask() == 0x07u) break;

                /* Đã báo xong các serie nhận được mà line vẫn im thêm 1 s ⇒
                 * hết đợt: kết thúc window để boot loop gửi REQ mới ngay thay
                 * vì ngồi im hết 30 s. */
                if (last_ack_ms != 0 &&
                    (uint32_t)(now_ms - last_ack_ms) >= 1000u) {
                    ESP_LOGI(TAG, "Boot sync: burst ended (mask=0x%02X) - re-requesting",
                             baseline_valid_mask());
                    break;
                }
            }
            continue;   /* nothing yet — keep waiting */
        }

        last_rx_ms = now_ms;

        /* Frame không gửi cho mình (downlink của node khác, hoặc uplink của node
         * khác nghe được trên kênh chung) → bỏ qua, không đổi state. */
        if (buf[0] != 0xFF && buf[0] != cfg->nodeId) {
            ESP_LOGD(TAG, "Boot sync: frame for node 0x%02X - ignoring", buf[0]);
            continue;
        }

        if (buf[1] == PKT_TYPE_BASELINE) {
            got_chunk = true;
            baseline_handle_chunk(buf, len);
            config_reset_gw_lost();

            /* KHÔNG commit/không phát 0x08 ở đây: phải chờ burst của gateway
             * phát xong (xem BASELINE_BURST_IDLE_MS ở nhánh timeout). */
            continue;
        }

        if (buf[1] == PKT_TYPE_CMD && len >= (int)sizeof(lora_cmd_packet_t)) {
            lora_cmd_packet_t cmd;
            memcpy(&cmd, buf, sizeof(cmd));
            if (cmd.crc != crc8_xor((uint8_t *)&cmd, sizeof(cmd) - 1)) {
                ESP_LOGW(TAG, "Boot sync: downlink CRC mismatch - ignoring");
                continue;
            }
            if (cmd.dest != 0xFF && cmd.dest != cfg->nodeId) {
                ESP_LOGD(TAG, "Boot sync: downlink for node 0x%02X - ignoring",
                         cmd.dest);
                continue;
            }
            config_set_current_slot(cmd.slot);
            commands_process(&cmd, NULL);
            config_reset_gw_lost();
            continue;
        }

        if (buf[1] == PKT_TYPE_ACK && len >= 8) {
            /* Chỉ ACK gửi cho MÌNH mới chứng minh gateway còn sống: ACK của
             * node khác cũng nghe được trên kênh chung. */
            lora_data_packet_t ack;
            memcpy(&ack, buf, sizeof(ack));
            if (ack.node_id == cfg->nodeId &&
                crc8_verify((uint8_t *)&ack, sizeof(ack)) == 0) {
                config_reset_gw_lost();
            } else {
                ESP_LOGD(TAG, "Boot sync: ACK for node 0x%02X - ignoring",
                         ack.node_id);
            }
            continue;
        }
    }
}

int commands_send_baseline_done(uint8_t series, uint8_t version)
{
    if (series >= BASELINE_SERIES_COUNT) return -1;
    return commands_send_baseline_done_mask((uint8_t)(1u << series), version);
}

int commands_send_baseline_done_mask(uint8_t series_mask, uint8_t version)
{
    app_config_t *cfg = config_get();
    uint8_t buf[8];

    series_mask &= 0x07u;
    if (series_mask == 0) return -1;

    /* Trường series mang bitmask (>=3) hoặc chỉ số serie 0/1/2 khi chỉ 1 bit. */
    uint8_t field = (series_mask == 0x01u) ? 0u :
                    (series_mask == 0x02u) ? 1u :
                    (series_mask == 0x04u) ? 2u : series_mask;

    buf[0] = cfg->nodeId;
    buf[1] = PKT_TYPE_BASELINE_DONE;   /* 0x08 */
    buf[2] = field;
    buf[3] = version;
    buf[4] = 0;
    buf[5] = 0;
    buf[6] = 0;
    buf[7] = crc8_xor(buf, 7);

    for (int i = 0; i < ACK_SEND_RETRIES; i++) {
        if (lora_send(buf, sizeof(buf)) == sizeof(buf)) {
            ESP_LOGI(TAG, "BASELINE_DONE sent (mask=0x%02X field=%u v%u)",
                     series_mask, field, version);
            return 0;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ESP_LOGW(TAG, "BASELINE_DONE transport failed (mask=0x%02X)", series_mask);
    return -1;
}