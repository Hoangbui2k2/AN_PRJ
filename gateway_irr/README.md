# gateway_irr — ESP32 LoRa-to-MQTT Irrigation Gateway

Firmware **ESP-IDF v5.3.1** cho ESP32 gateway cầu nối giữa các node tưới tiêu qua **LoRa UART** (SX1278) và **MQTT broker** (TLS).

```text
┌────────┐   LoRa UART (9600 baud)   ┌──────────┐    MQTT (TLS 8883)   ┌────────┐
│ Node 1 │ ───────────┬────────────► │ Gateway  │ ───────────────────► │ Server │
│ Node 2 │ ───────────┴────────────► │ (ESP32)  │ ◄─────────────────── │        │
│ ...    │  uplink 8B / compact 4-9B │          │   command / config   │        │
└────────┘ ◄────────── downlink ◄─────┘          └─────────────────────┘        │
```

---

## Tính năng

- **Cầu nối LoRa ↔ MQTT:** nhận uplink từ node (data, heartbeat, ack, alarm), publish lên MQTT; nhận lệnh MQTT từ server, forward xuống node.
- **Quản lý tối đa 11 nodes** (`MAX_NODES`), mặc định đăng ký 2 nodes (id 1, 2) từ config.
- **Cấp baseline tuần tự**: bảng baseline (tối đa 96 điểm/series × 3 series) được gửi cho **từng node một**. Node đang giữ "lượt" (turn) nhận chunk cho tới khi xong (hoặc ngừng uplink 120 s); các node khác xếp hàng FIFO và vẫn nhận ACK rỗng.
- **Command cache:** lệnh gửi cho node offline được cache (tối đa 20, FIFO eviction), tự động gửi khi node online trở lại.
- **Retry với exponential backoff:** 1s → 2s → 4s → 8s → 16s, tối đa 5 lần (`MAX_RETRY`).
- **Threshold-based reporting:** gateway kiểm tra độ ẩm đất (`soil`) so với ngưỡng `threshold_low/high`; publish alarm khi vượt ngưỡng hoặc khi node tự báo.
- **Mọi uplink đều được phản hồi downlink** (flush cache lệnh hoặc ACK rỗng `0x09`) — giúp node reset `gatewayLostCount`, tránh báo nhầm "gateway lost".
- **WiFi station tự động reconnect**, MQTT auto-reconnect, Last Will (`offline`).
- **Uplink compact (type `0x06`)**: chỉ gửi các field đã thay đổi theo presence bitmask, tiết kiệm LoRa traffic.

---

## Cấu trúc dự án

```text
gateway_irr/
├── CMakeLists.txt              # ESP-IDF project
├── sdkconfig.defaults          # Cấu hình sdk mặc định (WiFi sta, MQTT 3.1.1, FreeRTOS, ...)
├── main/
│   ├── main.c                  # Entry point + WiFi + xử lý uplink/command + 3 FreeRTOS tasks
│   ├── lora_uart.c/h           # UART2 LoRa: init, mode control, read/send packet, ACK
│   ├── mqtt.c/h                # MQTT client (TLS), topic subscribe, publish, event handler
│   ├── node_manager.c/h        # Bảng node: online/offline, sensor data, thresholds, deltas
│   ├── command_cache.c/h       # Cache lệnh offline + retry/backoff + theo dõi ACK
│   ├── config.c/h              # NVS config: WiFi, MQTT, identity (site/gw_id), default nodes
│   ├── topic.c/h               # Build & parse MQTT topic
│   └── crc.c/h                 # CRC8 (XOR all bytes)
├── agent.md                    # Cấu hình agent (tổng quan dự án, protocol, GPIO, flow)
├── skill.md                    # Hướng dẫn code theo đúng quy ước dự án
├── docs/
│   └── SYNC_NODE_THRESHOLD.md  # Yêu cầu đồng bộ threshold cho node firmware
├── MAPPING_GIAO_THUC_MQTT.md   # Chi tiết MQTT topics + JSON payload (cho server)
└── .vscode/                    # Cấu hình ESP-IDF (IDF path, port COM10)
```

---

## Yêu cầu phần cứng

|Component|Model|GPIO|Chức năng|
|---|---|---|---|
|MCU|ESP32 DevKit V1|—|—|
|LoRa Module|SX1278 UART (Ebyte)|—|Giao tiếp qua UART2|
|LoRa TX|—|**16**|UART2 TX|
|LoRa RX|—|**17**|UART2 RX|
|LoRa MD0|—|**22**|Mode control|
|LoRa MD1|—|**21**|Mode control|
|LoRa AUX|—|**19**|Ready/busy status|

---

## Cấu hình

Cấu hình lưu trong **NVS** (namespace `gateway_cfg`), khởi tạo mặc định trong `config.c`:

|Mục|Mặc định|
|---|---|
|Site / Gateway ID|`HCM` / `gw_01`|
|WiFi|SSID `Hoa Hau`, pass `12233445`|
|MQTT Broker|`mqtts://d246c46a2ebe40d2ae0c787f92bfdbab.s1.eu.hivemq.cloud` : `8883` (TLS)|
|MQTT User / Pass|`hivemq.webclient.1742180699133` / *(xem config.c)*|
|Nodes mặc định|2 nodes (id 1, 2), threshold 30–70%, report 300s, heartbeat 60s|

> ⚠️ Các giá trị mặc định chứa thông tin đăng nhập — đây là **giá trị dev**, không nên dùng trong production. Thay đổi trong `config.c` `config_set_defaults()` hoặc ghi đè qua NVS.

### Chỉnh sửa cấu hình (menuconfig)

```bash
idf.py menuconfig
```

Các lựa chọn quan trọng trong `sdkconfig.defaults`: WiFi station only, MQTT 3.1.1, FreeRTOS 100Hz 2 cores, log level INFO.

---

## Build & Flash

Yêu cầu: ESP-IDF **v5.3.1** (đã cài tại `C:/Espressif/frameworks/esp-idf-v5.3.1/`).

```bash
# Mở "ESP-IDF Terminal" trong VS Code (hoặc export.sh tương đương)
cd gateway_irr

idf.py build
idf.py -p COM10 flash
idf.py -p COM10 monitor        # theo dõi log (Ctrl+] để thoát)
```

Hoặc dùng VS Code Extension **Espressif IDF** (nút build/flash ở thanh dưới). Port mặc định `COM10` (`.vscode/settings.json`).

---

## MQTT Topics

Format: `irrigation/<site>/<gateway_id>/...`

|Hướng|Topic|Khi nào|
|---|---|---|
|Node → Server|`.../node_<hex>/data`|Mỗi uplink data|
|Node → Server|`.../node_<hex>/alarm`|Alarm từ node `0x04` hoặc gw phát hiện exceeded|
|Node → Server|`.../node_<hex>/status`|Node online/offline|
|Server → Node|`.../node_<hex>/cmd`|Lệnh điều khiển|
|GW → Server|`.../status`|Mỗi 60s + khi connect|
|GW → Server (LWT)|`.../status`|Khi mất kết nối|
|Server → GW|`.../config`|*(chưa dùng)*|

**Subscribe filter:** `irrigation/<site>/<gw>/+/cmd`

Chi tiết JSON payload và bảng lệnh: xem [MAPPING_GIAO_THUC_MQTT.md](MAPPING_GIAO_THUC_MQTT.md).

### Lệnh điều khiển (server → node)

```json
{"cmd":"on","duration":60}
{"cmd":"off"}
{"cmd":"set_threshold","low":30,"high":70}
{"cmd":"set_delta","type":2,"value":5}
{"cmd":"set_interval","value":300}
{"cmd":"report"}
```

---

## LoRa Protocol (tóm tắt)

- **Uplink legacy (8B):** `{node | type | flags | soil | temp | hum | battery | crc}` — type `0x01` data, `0x02` heartbeat, `0x03` ack, `0x04` alarm.
- **Uplink compact (4–9B):** `{node | 0x06 | presence | fields... | crc}` — field theo presence bitmask, temp wire = °C + 40.
- **Downlink (6B):** `{node | 0x05 | command | param1 | param2 | crc}` — header luôn `0x05`, command `0x09` = ACK rỗng.
- **CRC8 = XOR** tất cả bytes trước byte cuối.

Chi tiết: [agent.md](agent.md).

---

## Tài liệu liên quan

|File|Mô tả|
|---|---|
|[agent.md](agent.md)|Tổng quan kiến trúc, protocol, GPIO, flow logic (cho agent)|
|[skill.md](skill.md)|Hướng dẫn coding conventions cho agent|
|[MAPPING_GIAO_THUC_MQTT.md](MAPPING_GIAO_THUC_MQTT.md)|Mapping MQTT topics + JSON payloads|
|[docs/SYNC_NODE_THRESHOLD.md](docs/SYNC_NODE_THRESHOLD.md)|Yêu cầu đồng bộ threshold trên node firmware|
|[docs/superpowers/](docs/superpowers/)|Thiết kế & kế hoạch simulator|

---

## Trạng thái / Roadmap

- [x] Cầu nối LoRa ↔ MQTT (data, heartbeat, alarm, command)
- [x] Command cache + retry backoff + ACK tracking
- [x] Threshold & delta-based reporting (gateway side)
- [x] Uplink compact (type `0x06`)
- [x] Empty ACK (`0x09`) cho mọi uplink
- [ ] Remote config qua `.../config` (đổi broker/WiFi qua MQTT)
- [ ] Đồng bộ threshold & delta cho node firmware (`docs/SYNC_NODE_THRESHOLD.md`)
