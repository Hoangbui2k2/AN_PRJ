# Agent Configuration — ESP-IDF LoRa Irrigation Gateway

> Tài liệu cấu hình và hoạt động của agent này dành cho dự án **gateway_irr**.
> Mọi thông tin dưới đây được đối chiếu trực tiếp với code hiện tại trong `main/`.

---

## Project Overview

Firmware ESP32 (ESP-IDF) dùng làm **gateway cầu nối LoRa UART ↔ MQTT** cho hệ thống tưới tiêu.

- Quản lý tối đa **10 nodes** (`MAX_NODES`), mặc định khởi tạo **2 nodes** (`DEFAULT_NODE_COUNT`).
- Nhận uplink LoRa (data / heartbeat / ack / alarm) từ node, publish lên MQTT broker.
- Nhận command MQTT từ server, forward xuống node qua LoRa.
- **Cache commands** (`MAX_CACHED_COMMANDS = 20`) cho node đang offline, gửi khi node online trở lại.
- **Retry với exponential backoff**: 1s → 2s → 4s → 8s → 16s (`MAX_RETRY = 5`).
- Cơ chế **threshold-based reporting**: cả node và gateway cùng kiểm tra độ ẩm đất so với ngưỡng.

---

## Architecture — Các module trong `main/`

| File | Trách nhiệm |
|---|---|
| [main.c](main/main.c) | Entry point: WiFi station, xử lý uplink LoRa, JSON payloads, MQTT command, 3 FreeRTOS tasks |
| [lora_uart.c](main/lora_uart.c) | Giao tiếp UART2 với module LoRa SX1278: init, mode control (MD0/MD1), read/send packet, ACK |
| [mqtt.c](main/mqtt.c) | Client MQTT (esp_mqtt_client), connect TLS, subscribe topic, publish, event handler |
| [node_manager.c](main/node_manager.c) | Bảng trạng thái nodes (online/offline, last_seen, sensor data, thresholds, deltas) |
| [command_cache.c](main/command_cache.c) | Cache lệnh cho node offline, retry với backoff, theo dõi in-flight ACK |
| [config.c](main/config.c) | NVS: WiFi, MQTT, identity (site/gateway_id), default nodes |
| [topic.c](main/topic.c) | Xây dựng & parse MQTT topic theo format `irrigation/<site>/<gw>/...` |
| [crc.c](main/crc.c) | CRC8 = XOR all bytes |
| [crc.h](main/crc.h) | — |

**Pipeline chính:**

```
Node ──(LoRa uplink 8B / compact)──► lora_uart.c ──► main.c process_node_packet()
      ──(6B downlink)──► lora_uart.c                    │
                                                       ├──► node_manager.c (update + threshold)
                                                       ├──► mqtt.c publish (data/alarm/status/gw-status)
                                                       └──► ack_or_flush_node() (flush cache hoặc ACK rỗng)

Server ──(MQTT cmd)──► mqtt.c ──► handle_mqtt_command() ──► lora_send_command() hoặc command_cache_add()
```

---

## Hardware Specifications

| Component | Model | Notes |
|---|---|---|
| MCU | ESP32 DevKit V1 | 5V USB, ESP-IDF v5.3.1 |
| LoRa Module | SX1278 UART (Ebyte) | UART2, 9600 baud |

### GPIO Mapping (THEO CODE HIỆN TẠI — `lora_uart.h`)

| Function | GPIO | Mode | Notes |
|---|---|---|---|
| LoRa TX (ESP32 → LoRa) | **16** | Output | UART2 TX |
| LoRa RX (LoRa → ESP32) | **17** | Input | UART2 RX |
| LoRa MD0 | **22** | Output | Mode control |
| LoRa MD1 | **21** | Output | Mode control |
| LoRa AUX | **19** | Input | Ready/busy status |

> ⚠️ Khác với agent.md cũ (ghi MD0=23, MD1=22). Đây là giá trị **đúng theo `main/lora_uart.h:13-18`**.

### LoRa Mode Control (`lora_mode_t`)

| Mode | MD0 | MD1 |
|---|---|---|
| NORMAL (hoạt động) | 0 | 0 |
| CONFIG | 1 | 0 |
| WOR | 0 | 1 |
| DEEP_SLEEP | 1 | 1 |

---

## LoRa Packet Protocol (`lora_uart.h`)

### Uplink (Node → Gateway)

**Legacy — 8 bytes** (types `0x01` data, `0x02` heartbeat, `0x03` ack, `0x04` alarm):

```
{ node_id | type | flags | soil | temp(s8) | hum | battery | crc }
```

**Compact — 4..9 bytes** (type `0x06` data compact, `presence` ở byte 2):

```
{ node_id | 0x06 | presence | [fields theo presence] | crc }
```

Payload compact theo LSB-first: temp → hum → soil → battery → flags. `temp` nhận qua `stored - 40` (offset +40 → °C).

**Presence bitmask (`0x1F`, tối đa 5 field):**

| Bit | Constant | Meaning |
|---|---|---|
| 0 | `PRESENCE_TEMPERATURE` | temp (1 byte, +40) |
| 1 | `PRESENCE_HUMIDITY` | hum |
| 2 | `PRESENCE_SOIL_MOIST` | soil |
| 3 | `PRESENCE_BATTERY` | battery |
| 4 | `PRESENCE_FLAGS` | flags |

**Flags (byte flags, cả legacy & compact):**

| Bit | Constant | Meaning |
|---|---|---|
| 0 | `FLAG_PUMP_ON` | Pump đang chạy |
| 1 | `FLAG_THRESHOLD_EXCEEDED` | Soil outside threshold band |
| 2 | `FLAG_GW_LOST` | Node báo mất gateway |
| 3 | `FLAG_SENSOR_OK` | Cảm biến OK |

### Downlink (Gateway → Node) — luôn 6 bytes

```
Byte 0: node_id | Byte 1: header 0x05 | Byte 2: command | Byte 3: param1 | Byte 4: param2 | Byte 5: crc
```

Mọi uplink đều được phản hồi trong downlink (`ack_or_flush_node`) — gửi lệnh cache pending hoặc ACK rỗng.

**Command bytes:**

| Value | constant | Param1 | Param2 |
|---|---|---|---|
| 0x01 | `LORA_CMD_SET_INTERVAL` | report interval (s) | — |
| 0x02 | `LORA_CMD_ON` | duration (s) | — |
| 0x03 | `LORA_CMD_OFF` | — | — |
| 0x04 | `LORA_CMD_SET_THRESHOLD` | low (%) | high (%) |
| 0x05 | `LORA_CMD_SET_SCHEDULE` | hour | minute |
| 0x06 | `LORA_CMD_REPORT` | — | — |
| 0x07 | `LORA_CMD_TOGGLE` | — | — |
| 0x08 | `LORA_CMD_SET_MODE` | mode | — |
| 0x09 | `LORA_CMD_ACK` (empty ACK) | — | — |
| 0x0A | `LORA_CMD_SET_DELTA` | type | value |

**Delta types (`set_delta` param1 / `DELTA_TYPE_*`):** 0=temp (×10 → °C), 1=hum (%), 2=soil (%), 3=battery (%).

---

## MQTT Topics (`topic.c`)

Format chung: `irrigation/<site>/<gateway_id>/...`

| Type | Topic | Hướng | Khi nào publish |
|---|---|---|---|
| `TOPIC_NODE_DATA` | `.../node_<hex>/data` | Node → Server | Mỗi uplink data |
| `TOPIC_NODE_ALARM` | `.../node_<hex>/alarm` | Node → Server | Alarm packet `0x04` hoặc gw phát hiện exceeded |
| `TOPIC_NODE_STATUS` | `.../node_<hex>/status` | Node → Server | Node online/offline |
| `TOPIC_NODE_CMD` | `.../node_<hex>/cmd` | Server → Node | Subscribe filter: `.../+/cmd` |
| `TOPIC_GW_STATUS` | `.../status` | GW → Server | Mỗi 60s + khi MQTT connected |
| `TOPIC_GW_WILL` | `.../status` | LWT | Khi mất MQTT: `{"status":"offline"}` |
| `TOPIC_GW_CONFIG` | `.../config` | Server → GW | *(chưa dùng)* |

**`topic_build()`**: node level → `irrigation/<site>/<gw>/node_%02X/<suffix>`; gateway level → `irrigation/<site>/<gw>/<suffix>`.  
**Subscribe filter**: `irrigation/<site>/<gw>/+/cmd`.  
Chi tiết JSON payload: xem [MAPPING_GIAO_THUC_MQTT.md](MAPPING_GIAO_THUC_MQTT.md).

---

## Core Logic Flows

### 1. Xử lý uplink LoRa (`process_node_packet`)

- **`0x03` ACK**: xóa in-flight command khỏi cache (tìm bằng `command_cache_get_inflight`), không cần truyền command bytes chính xác.
- **`0x02` Heartbeat**: `node_mark_online()`, publish status **online nếu** node trước đó offline, **không update sensor data**, luôn trả lời downlink.
- **`0x04` Alarm**: `alarm_code = flags`, cross-verify với `soil` (redundant copy), publish alarm có `alarm_code` + `message`. Chỉ mark online — **không ghi đè data sensor**. Alarm fire-and-forget, không ACK chờ.
- **Data (`0x01`/`0x02` legacy, `0x06` compact)**: 
  - `0x06` compact: merge với `last_data` node, chỉ ghi đè field có trong presence.
  - Update node data → `publish_node_data()` luôn luôn publish → `node_update_last_reported()` (baseline delta).
  - Threshold: kết quả `(node flag THRESHOLD_EXCEEDED) || (gateway check soil so vs threshold)` → publish alarm nếu exceeded.
  - Cuối cùng luôn gọi `ack_or_flush_node` (phản hồi downlink).

### 2. `ack_or_flush_node(node_id)` — phản hồi mọi uplink

- Có command cache cho node → gửi `send_cached_commands_for_node` (FIFO, mỗi command vào trạng thái waiting-ACK).
- Không có → `lora_send_ack()` (empty ACK `0x09`) để node reset `gatewayLostCount`, tránh alarm `0x05` "Gateway lost".

### 3. MQTT Command (`handle_mqtt_command`)

1. Parse JSON, đọc `cmd` string → map sang byte command (`map_command_to_lora_byte`). Không biết `0x00` → discard.
2. `get_node_defaults` — lấy ngưỡng/deltas/interval theo live node, else fallback config NVS (`node_config_t`).
3. Build `param1/param2` lệ theo từng command; `set_threshold` & `set_delta` **update local cache ngay** (checks chạy đúng ngay khi chưa có LoRa ACK).
4. Node online → `lora_send_command`, retry `MAX_RETRY` (1/2/4/8/16s); thất bại sau 5 lần → build packet 6-byte + `crc8_calculate` → cache.
5. Node offline → `command_cache_add` ngay.

### 4. Command Cache (`command_cache.c`)

- FIFO theo `next_retry_ms`. Dedup: không cache lệnh trùng byte-đã có.
- Cache đầy → FIFO eviction (replace oldest pending).
- In-flight: `command_cache_mark_sent` → `waiting_ack = true`; ACK timeout `ACK_TIMEOUT_MS = 3000` → retry hoặc drop khi `retry_count >= MAX_RETRY` (trong `command_cache_get_retry_ready`, gọi từ `cmd_retry_task` hàng 500ms).
- Trạng thái node offline khi cmd_retry_task gọi node offline → `command_cache_advance_retry`.

### 5. Node timeout

- `NODE_TIMEOUT_MS = 180000` (3 phút). `node_check_timeouts()` chạy từ `lora_rx_task` mỗi `NODE_MONITOR_PERIOD_MS = 1000`, trả bitmask → publish offline status.

### 6. Các FreeRTOS tasks (khởi tạo trong `app_main`)

| Task | Stack | Priority | Period | Chức năng |
|---|---|---|---|---|
| `lora_rx_task` | 4096 | 5 | 10ms poll; 1s timeout check | Đọc UART → process packet; check timeout; log status 30s |
| `cmd_retry_task` | 3072 | 4 | 500ms | Retry queue: node online → send + mark_sent, else advance_retry |
| `gateway_status_task` | 4096 | 5 | 60s | Publish GW status JSON |

---

## Configuration — NVS (`config.c`)

- **NVS namespace**: `gateway_cfg`. Keys: `site`, `gw_id`, `wifi_ssid`, `wifi_pass`, `mqtt_uri`, `mqtt_user`, `mqtt_pass`, `mqtt_port`.
- **Default (`config_set_defaults`)**: site = `HCM`, gateway_id = `gw_01`, WiFi `Hoa Hau`/`12233445`, broker `mqtts://d246c46a2ebe40d2ae0c787f92bfdbab.s1.eu.hivemq.cloud` port **8883** (TLS).
- 2 default nodes: id=1, id=2; threshold 30–70 (`THRESHOLD_LOW/HIGH_DEFAULT`).
- `config_init` gọi trong `app_main` trước WiFi/MQTT/LoRa. MQTT dùng `.crt_bundle_attach = esp_crt_bundle_attach` để verify TLS.

---

## Build Environment

- **IDF path:** `C:/Espressif/frameworks/esp-idf-v5.3.1/` (mặc định VS Code `.vscode/settings.json`).
- **Board:** ESP32, flash type UART, port `COM10`.
- `sdkconfig.defaults`: WiFi station only (SOFTAP off), MQTT 3.1.1 (SSL off; TLS qua broker URI `mqtts://` + bundle), FreeRTOS 2 cores @100Hz, log level default 3, disabled WDT, heap poison disable.

### Common commands (ESP-IDF on Windows)

```bash
idf.py build
idf.py -p COM10 flash monitor   # hoặc dùng ESP-IDF Terminal trong VS Code
```

---

## Testing / Docs Related

- Kết hợp với `gateway_simulator` (docs: [docs/superpowers/...](docs/superpowers/)).
- [docs/SYNC_NODE_THRESHOLD.md](docs/SYNC_NODE_THRESHOLD.md) — yêu cầu đồng bộ threshold cho node firmware (cần implement trên node).
- MAPPING_GIAO_THUC_MQTT.md — mô tả chính thức cho server.

---

## Gotchas / Notes

- Node ID được in hex trong topic (`node_XX` với XX hex). MQTT topic `cmd` parse node_id bằng `topic_parse()` (hex).
- `lora_send_command` header luôn `0x05`; command `0x09` là ACK rỗng không thực thi.
- **agent.md cũ ghi sai GPIO**: MD0=23, MD1=22. Code hiện tại dùng **MD0=22, MD1=21** (`lora_uart.h`).
- `get_node_defaults` fallback: nếu node chưa registered, match theo `s_config.nodes` theo id, else dùng `nodes[0]`. Với 2 default nodes trong config, server chỉ nên gửi command cho các node đã đăng ký.