# Skill: ESP-IDF Firmware cho LoRa Irrigation Gateway

> Hướng dẫn agent viết/sửa code cho **gateway_irr** đúng theo cấu trúc và quy ước hiện tại.
> **Luôn đọc trước các header trong `main/` (`lora_uart.h`, `node_manager.h`, `command_cache.h`, `topic.h`, `config.h`) để nắm đúng API và hằng số.**

---

## Context

Firmware C (ESP-IDF) cho ESP32 gateway cầu nối LoRa UART ↔ MQTT. Quản lý ≤ 10 nodes, cache lệnh cho node offline (tối đa 20), retry lệnh 5 lần với exponential backoff, threshold-based reporting.

**Framework:** ESP-IDF v5.3.1 (`C:/Espressif/frameworks/esp-idf-v5.3.1/`). **Platform:** ESP32 DevKit V1, UART flash `COM10`.

---

## Code Style & Conventions

- Dùng quy ước ESP-IDF: `esp_log` với `TAG` (viết HOA, ≤ 10 ký tự, ví dụ `"LORA_UART"`, `"CMD_CACHE"`, `"NODE_MGR"`).
- Xử lý lỗi: `ESP_ERROR_CHECK` cho boot-critical, `esp_err_t` return cho module, log `ESP_LOGW/ESP_LOGE` + trả về sớm.
- FreeRTOS tasks cho LoRa RX và MQTT; **tránh blocking** — dùng timeout (`pdMS_TO_TICKS`) và `vTaskDelay`.
- Struct payload nhị phân đóng gói với `__attribute__((packed))`.
- Các hằng số protocol tập trung trong header (`lora_uart.h`) — **không hardcode byte ma thuật trong .c**.
- Sử dụng JSON qua `cJSON` (không dùng `snprintf` thủ công cho payload khi có cấu trúc).
- CRC8 = **XOR toàn bộ bytes** (`crc8_calculate`), không phải polynomial chuẩn.

---

## Cấu trúc module & Luồng gọi chính

```
main/
├── main.c            app_main + 3 tasks (lora_rx, cmd_retry, gateway_status) + process_node_packet + handle_mqtt_command
├── lora_uart.[ch]    UART2 (9600 baud) + mode control MD0/MD1 + read/send packet
├── mqtt.[ch]         esp_mqtt_client wrapper + event handler + topic subscribe
├── node_manager.[ch] node_entry_t table + threshold/delta logic + timeout
├── command_cache.[ch] cached_command_t FIFO + retry/backoff + in-flight ACK tracking
├── config.[ch]       NVS (gateway_cfg) + defaults (2 nodes)
├── topic.[ch]        topic_build / topic_parse / cmd filter
└── crc.[ch]          CRC8 = XOR
```

`main.c` orchestrate toàn bộ. Khi thêm tính năng: **viết logic trong module riêng (`.c`/`.h`), gọi từ `main.c` hoặc task tương ứng.**

---

## LoRa Protocol — Ghi nhớ khi code

### Uplink (Node → Gateway)
- **Legacy 8 bytes** (`PKT_TYPE_DATA 0x01`, `HEARTBEAT 0x02`, `ACK 0x03`, `ALARM 0x04`):
  `{ node_id | type | flags | soil | temp(int8) | hum | battery | crc }`
- **Compact `0x06` (4–9 bytes)**: byte 2 = `presence` bitmask (`0x1F`), payload LSB-first theo presence: **temp → hum → soil → battery → flags**, kết thúc bằng CRC.
  - `temp` wire format: `stored = temp_C + 40` → đọc phải `payload[off] - 40`.
  - Kích thước: `DATA_COMPACT_HEADER 3` + N payload + 1 CRC, tối đa `DATA_COMPACT_MAX_SIZE 9`.
  - CRC phủ header (3 bytes) + payload thật (`crc8_calculate(verify_buf, 3 + payload_count)`).
- `lora_read_packet()` tự xử lý cả 2 loại dựa trên `type` — agent chỉ parse `lora_uplink_packet_t` đã fill sẵn.

### Downlink (Gateway → Node) — 6 bytes
`{ node_id | 0x05 | command | param1 | param2 | crc }`

| Command | value | Param1 | Param2 |
|---|---|---|---|
| `LORA_CMD_SET_INTERVAL` | 0x01 | interval (s) | — |
| `LORA_CMD_ON` | 0x02 | duration (s) | — |
| `LORA_CMD_OFF` | 0x03 | — | — |
| `LORA_CMD_SET_THRESHOLD` | 0x04 | low (%) | high (%) |
| `LORA_CMD_SET_SCHEDULE` | 0x05 | hour | minute |
| `LORA_CMD_REPORT` | 0x06 | — | — |
| `LORA_CMD_TOGGLE` | 0x07 | — | — |
| `LORA_CMD_SET_MODE` | 0x08 | mode | — |
| `LORA_CMD_ACK` (rỗng) | 0x09 | — | — |
| `LORA_CMD_SET_DELTA` | 0x0A | type | value |

**Quy tắc phản hồi downlink:** MỌI uplink đều phải có downlink reply. Dùng `ack_or_flush_node()`: có command cache → flush FIFO; không → `lora_send_ack()` (0x09). Điều này để node reset `gatewayLostCount` và tránh alarm `0x05`.

---

## Node Manager — API cốt lõi

- `node_find_or_create(id)` / `node_find(id)` — lookup theo `node_id` (uint8_t).
- `node_update_data(id, soil, temp, hum, battery, pump_state, flags)` — ghi sensor + mark online + update last_seen.
- `node_mark_online(id)` / `node_mark_offline(id)` / `node_mark_online` không ghi data.
- `node_check_threshold(id, soil)` — trả `true` nếu `soil < low || soil > high`, set `threshold_exceeded`.
- `node_set_threshold(id, low, high)` — **clamp 0..100, reject `low >= high`**.
- `node_set_delta_threshold(id, type, value)` — type theo `DELTA_TYPE_*`.
- `node_update_last_reported(id)` — baseline cho delta comparison, gọi sau khi publish data.
- `node_check_timeouts()` — trả bitmask nodes vừa offline (`NODE_TIMEOUT_MS = 180000`).
- Hằng số default: `THRESHOLD_LOW_DEFAULT 30`, `THRESHOLD_HIGH_DEFAULT 70`, `REPORT_INTERVAL_DEFAULT 300`, `DELTA_TEMP_DEFAULT 20` (=2.0°C, ×10).

**Khi viết code dùng node:** đừng gọi `node_find` rồi dùng pointer lâu — `node_update_data` có thể tạo mới node. Nếu cần cả state trước/sau, gọi `node_find` lại sau khi update (xem pattern trong `process_node_packet`).

---

## Command Cache — API cốt lõi

- `command_cache_add(node_id, cmd_pkt)` — dedup byte-trong, FIFO eviction khi đầy (20).
- `command_cache_get_next(node_id)` — lệnh FIFO **không** đang waiting_ack (chỉ 1 in-flight/node).
- `command_cache_get_inflight(node_id)` — lệnh đang chờ ACK.
- `command_cache_mark_sent(cmd)` — set `waiting_ack`, ghi `last_sent_ms`.
- `command_cache_get_retry_ready()` — xử lý ACK timeout (`ACK_TIMEOUT_MS = 3000`) + trả lệnh đến hạn retry; drop khi `retry_count >= MAX_RETRY`.
- `command_cache_advance_retry(cmd)` — tăng retry, backoff `1000 << (count-1)` capped 16000ms.
- `command_cache_remove(cmd)` — gọi khi ACK nhận được (trong `process_node_packet` case ACK).

**Ví dụ retry pattern đúng (từ `cmd_retry_task`):**
```c
cached_command_t *cmd = command_cache_get_retry_ready();
if (cmd) {
    node_entry_t *node = node_find(cmd->node_id);
    if (node && node->online) {
        if (lora_send_command(cmd->cmd[0], cmd->cmd[2], cmd->cmd[3], cmd->cmd[4])) {
            command_cache_mark_sent(cmd);          /* chờ node ACK */
        } else {
            command_cache_advance_retry(cmd);
        }
    } else {
        command_cache_advance_retry(cmd);          /* node còn offline */
    }
}
```

---

## MQTT — Ghi nhớ khi code

- `mqtt_app_init(broker_uri, username, password, port, site, gateway_id)` — gọi trước, tự reconnect.
- `mqtt_set_command_callback(cb)` — cb dạng `void cb(uint8_t node_id, const char *payload, size_t len)`.
- `mqtt_publish(topic, payload, qos)` — trả -1 nếu chưa connect; **luôn check `mqtt_is_connected()` trước khi build/publish**.
- Payload JSON build bằng `cJSON`; `timestamp` dùng ISO 8601 UTC.
- **Node ID trên topic là hex:** `node_%02X`. Khi parse topic → `topic_parse()` trả `info.node_id` (hex).
- Chỉ dispatch command khi topic chứa `/cmd` (check trong `mqtt_event_handler`).

**Subscribe khi connect (mẫu từ mqtt.c):**
```c
/* trong MQTT_EVENT_CONNECTED */
topic_get_node_cmd_filter(topic, sizeof(topic), s_site, s_gateway_id);
esp_mqtt_client_subscribe(s_mqtt_client, topic, 1);   /* irrigation/<site>/<gw>/+/cmd */
topic_build(topic, sizeof(topic), s_site, s_gateway_id, TOPIC_GW_CONFIG, 0);
esp_mqtt_client_subscribe(s_mqtt_client, topic, 1);
```

---

## MQTT Command Mapping (server → node)

Trong `handle_mqtt_command` (main.c), chuẩn hóa theo bảng:

| JSON `cmd` | → LoRa byte | Đọc param |
|---|---|---|
| `set_interval` | 0x01 | `value` (else default report_interval) |
| `on` | 0x02 | `duration` |
| `off` | 0x03 | — |
| `set_threshold` | 0x04 | `low`, `high` (else defaults) → **cũng gọi `node_set_threshold` local** |
| `set_schedule` | 0x05 | `hour`, `minute` |
| `report` | 0x06 | — |
| `toggle` | 0x07 | — |
| `set_mode` | 0x08 | `mode` (else default) |
| `set_delta` | 0x0A | `type` (else `DELTA_TYPE_SOIL`), `value` (else default theo type) → **cũng gọi `node_set_delta_threshold` local** |

Quy tắc:
1. `map_command_to_lora_byte()` trả `0x00` cho command không biết → discard + log.
2. Luôn lấy defaults qua `get_node_defaults(node_id, &defaults)` trước khi build param.
3. Khi node **online**: `lora_send_command` + retry (1/2/4/8/16s). Khi thất bại 5 lần hoặc node **offline**: build `cmd_pkt[6]` với header `0x05` + `crc8_calculate` → `command_cache_add`.

---

## Threshold Reporting — Logic kết hợp

- Node tự báo `FLAG_THRESHOLD_EXCEEDED` (bit 1) hoặc gửi alarm `0x04`.
- Gateway **luôn verify lại**: `node_check_threshold(node_id, soil)`.
- Publish data **luôn luôn** (kể cả trong ngưỡng) → `publish_node_data()`.
- Alarm: `(node flag) || (gateway check)` → `publish_node_alarm()`. Alarm packet `0x04` dùng `publish_node_alarm_code()` (khác payload) và **không** tạo alarm thứ hai.
- Alarm `0x04` không ghi đè sensor data — chỉ mark online.

---

## Topic Format

```
irrigation/<site>/<gateway_id>/node_<node_id_hex>/data|alarm|status|cmd
irrigation/<site>/<gateway_id>/status|config
```

- `topic_build(buf, size, site, gw_id, type, node_id)` — tự thêm `node_%02X` cho node topics.
- `topic_get_node_cmd_filter()` — `irrigation/<site>/<gw>/+/cmd`.
- Khi thêm topic mới: thêm enum vào `topic_type_t` (topic.h) + suffix trong `topic_suffix()` (topic.c).

---

## Config / NVS

- NVS namespace `gateway_cfg`, keys: `site`, `gw_id`, `wifi_ssid`, `wifi_pass`, `mqtt_uri`, `mqtt_user`, `mqtt_pass`, `mqtt_port`.
- `config_set_defaults()` cần cập nhật khi thay đổi default (site, broker, nodes).
- `node_config_t` — defaults cho 2 nodes (id 1, 2), dùng khi server gửi command thiếu param.
- **Khi thêm tham số cấu hình mới:** thêm field vào `gateway_config_t` + key NVS + load trong `config_init` + print trong `config_print`.

---

## FreeRTOS Tasks (đã có)

| Task | Stack | Prio | Period | Lưu ý khi sửa |
|---|---|---|---|---|
| `lora_rx_task` | 4096 | 5 | 10ms poll, timeout check 1s | Đừng blocking quá lâu trong vòng lặp (đọc UART dùng timeout) |
| `cmd_retry_task` | 3072 | 4 | 500ms | Chỉ gọi `command_cache_get_retry_ready` + send |
| `gateway_status_task` | 4096 | 5 | 60s | Check `mqtt_is_connected()` trước publish |

---

## Debugging Tips

```c
#define TAG "GATEWAY"

/* Monitor LoRa traffic — sau lora_read_packet */
ESP_LOGI(TAG, "LoRa RX: %02X %02X %02X %02X %02X %02X %02X %02X",
         packet.node_id, packet.type, packet.flags, packet.soil,
         packet.temp, packet.hum, packet.battery, packet.crc);

/* Monitor MQTT publish */
ESP_LOGI(TAG, "MQTT publish [%s]: %s", topic, payload);
```

- Theo dõi trạng thái node: `node_print_all()`; cache: `command_cache_print()` — cả hai tự log mỗi 30s trong `lora_rx_task`.
- Log level mặc định `CONFIG_LOG_DEFAULT_LEVEL=3` (INFO). Dùng `ESP_LOGD` cho chi tiết (bật DEBUG trong `idf.py menuconfig` nếu cần).

---

## Checklist khi thêm tính năng

1. Đọc các header liên quan (`lora_uart.h`, `node_manager.h`, `command_cache.h`, `topic.h`, `config.h`).
2. Thêm hằng số/enum vào header đúng chỗ (không hardcode trong .c).
3. Implement logic trong module phù hợp; `main.c` chỉ orchestrate.
4. Nếu đổi protocol LoRa/MQTT payload → cập nhật MAPPING_GIAO_THUC_MQTT.md + docs.
5. Build thử: `idf.py build` (ESP-IDF Terminal trong VS Code).
