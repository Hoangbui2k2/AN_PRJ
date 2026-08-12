# Test Cases — Server ↔ Gateway ↔ Node (IT & System Test)

> Hệ thống tưới tiêu thông minh: **Node** (firmware ESP32) ⟷ (LoRa UART) ⟷ **Gateway** (firmware ESP32) ⟷ (MQTT TLS) ⟷ **Server**.
> Tài liệu cung cấp bộ **test case IT (integration) + System test** với **Input / Output chi tiết** cho cả 2 tầng giao tiếp.

| Thành phần | Firmware | Cổng serial | Giao tiếp |
|---|---|---|---|
| Node | `node_irr` | UART dây LoRa | LoRa UART 9600 baud |
| Gateway | `gateway_irr` | `COM10` | LoRa UART + MQTT (TLS 8883) |
| Server (mô phỏng) | `test/mqtt_server_sim.py` | — | MQTT broker HiveMQ Cloud |

---

## 0. Cách sử dụng tài liệu

Mỗi test case có định dạng chuẩn:

- **Loại**: `IT` (giữa 2 module) hoặc `System` (end-to-end cả 3 thành phần).
- **Tầng**: `Node↔GW`, `GW↔Server`, hoặc `End-to-end` (Server→GW→Node→GW→Server).
- **Input**: dữ liệu đưa vào **chính xác** — byte LoRa (hex) hoặc JSON MQTT.
- **Output mong đợi**: byte/JSON/message + **điều kiện PASS**.
- **Verify**: cách xác nhận (serial node / serial gateway / bảng nhận trên script mô phỏng server).

### Quy ước byte gói tin

**Uplink LoRa 8B (Node → Gateway):**

```
| node_id | type | flags | soil | temp(+40) | hum | battery | crc |
```

- `temp` lệch +40: `temp_byte = temperature + 40`. VD 28°C → `0x44` (68).
- `crc` = XOR của 7 byte trước (không bỏ 0x00).

**Downlink LoRa 6B (GW → Node):**

```
| node_id | 0x05 | cmd | param1 | param2 | crc |
```

- `crc` = XOR của 5 byte trước.

**LoRa packet types (uplink):** `0x01` data, `0x02` heartbeat, `0x03` ACK, `0x04` alarm, `0x06` compact.

**Flags (uplink):** `0x01`=pump on, `0x02`=threshold_exceeded, `0x04`=GW_lost, `0x08`=sensor_ok.

**Compact presence:** `0x01`=temp, `0x02`=hum, `0x04`=soil, `0x08`=battery, `0x10`=flags.

**Command codes (downlink):** `0x01` set_interval, `0x02` on, `0x03` off, `0x04` set_threshold, `0x05` set_schedule, `0x06` report, `0x07` toggle, `0x08` set_mode, `0x09` ACK rỗng/keep-alive, `0x0A` set_delta.

**Alarm codes:** `0x01` sensor error, `0x02` soil out range, `0x03` relay error, `0x04` low battery, `0x05` gateway lost.

**Alert message map (MAPPING_GIAO_THUC_MQTT.md):**

| Code | Message |
|---|---|
| 1 | `Sensor error — 3 consecutive sensor read failures` |
| 2 | `Soil out of range — soil moisture < 10% or > 90%` |
| 3 | `Relay error — pump relay fault (pulse or GPIO)` |
| 4 | `Low battery — battery < 20%` |
| 5 | `Gateway lost — node lost connection (>=3 ACK timeouts)` |

### MQTT topics (site/gw mặc định `HCM` / `gw_01`)

| Hướng | Topic | Payload |
|---|---|---|
| Node→Server | `irrigation/HCM/gw_01/node_01/data` | JSON sensor |
| Node→Server | `irrigation/HCM/gw_01/node_01/status` | `{"node":1,"status":"online/offline",...}` |
| Node→Server | `irrigation/HCM/gw_01/node_01/alarm` | JSON alarm |
| GW→Server | `irrigation/HCM/gw_01/status` | JSON heartbeat |
| Server→Node | `irrigation/HCM/gw_01/node_01/cmd` | JSON lệnh |
| Server→GW | `irrigation/HCM/gw_01/config` | *(chưa dùng — ngoài phạm vi)* |

---

## Chuẩn bị môi trường (chạy 1 lần trước mọi nhóm)

1. Flash firmware **Node** (`node_irr`), mở serial monitor.
2. Flash firmware **Gateway** (`gateway_irr`), mở serial monitor (`COM10`).
3. Bật **MQTT broker** (HiveMQ Cloud theo `config.c`, TLS 8883).
4. Chạy script mô phỏng server:
   ```bash
   cd test
   python mqtt_server_sim.py --broker mqtts://d246c46a2ebe40d2ae0c787f92bfdbab.s1.eu.hivemq.cloud \
     --port 8883 --user <user> --pass <pass> --site HCM --gw gw_01 --nodes 1,2
   ```
5. Xác nhận cả 2 node `ONLINE` + gateway `ONLINE` trên bảng script.

> Log gateway tag `GATEWAY`, node tag `MAIN`/`COMMANDS`. Khi input không ghi cụ thể, giá trị sensor lấy theo thực tế.

---

## NHÓM A — Bootstrap (khởi động hệ thống)

### TC-A01 | Node cold boot — baseline data đầu tiên
- Loại: IT | Tầng: Node↔GW
- Tiền điều kiện: Node lần đầu cấp nguồn (RTC magic invalid); gateway + MQTT online.
- Bước: cấp nguồn Node (**Input**: không, cold boot / wake=UNDEFINED).
- Output mong đợi:
  - Node: LED blink 3 lần; log `Cold boot (first power-on)`.
  - Vì `lastSentTemp == INT8_MIN` → node gửi **baseline 0x01 đầy đủ** (VD: `01 01 08 2E 44 41 55 <crc>` — soil 46, temp 28, hum 65, bat 85, flags=0x08 sensor_ok).
  - Gateway publish `.../node_01/data` JSON + trả downlink ACK rỗng `01 05 09 00 00 <crc>`.
  - Node: log `gatewayLostCount reset to 0 (valid downlink)`.
- **PASS**: (1) baseline 0x01 gửi lần đầu, (2) data JSON khớp node/soil/temp/hum/battery, (3) ACK rỗng về node trong window 350ms.
- Verify: serial node + script nhận data.

### TC-A02 — Gateway boot — publish online ngay khi connect MQTT
- Loại: IT | Tầng: GW↔Server | Input: bật gateway.
- Output mong đợi: ngay sau `MQTT_EVENT_CONNECTED`, gateway publish:
  `{"status":"online","version":"1.0.0"}` và subscribe `irrigation/HCM/gw_01/+/cmd`.
- **PASS**: script nhận online ngay sau khi gateway connect.

### TC-A03 — Gateway heartbeat 60s định kỳ
- Loại: IT | Tầng: GW↔Server | Mỗi 60s trên `.../gw_01/status`:
  `{"status":"online","nodes_registered":2,"commands_cached":0,"uptime_seconds":<tăng>,"version":"1.0.0","site":"HCM","gateway_id":"gw_01"}`
- **PASS**: 2 heartbeat cách ~60s, `uptime_seconds` tăng, `nodes_registered=2`, `commands_cached=0`.
- Verify: script nhận 2 message trong 130s.

### TC-A04 — LWT offline khi mất kết nối MQTT
- Loại: System | Tầng: GW↔Server | Input: tắt broker (hoặc ngắt mạng gateway).
- Output: broker publish Last Will `{"status":"offline"}` lên `.../gw_01/status`.
- **PASS**: script nhận offline trong < ~1 phút.
- Note: LWT do broker xử lý; phụ thuộc broker có ghi nhận.

---

## NHÓM B — Node → Gateway (LoRa uplink)

### TC-B01 | Data packet đầy đủ 0x01 → publish data JSON
- Loại: IT | Tầng: Node↔GW | Input (uplink 8B ví dụ):
  `01 01 00 32 44 41 55 62`
  (node=1, type=0x01, flags=0, soil=0x32(50), temp=0x44(68→28°C), hum=0x41(65), batt=0x55(85); crc=XOR(01^01^00^32^44^41^55)=0x62)
- Output mong đợi:
  - `.../node_01/data`: `{"node":1,"soil":50,"temp":28,"hum":65,"pump":0,"battery":85,"threshold_exceeded":false,"timestamp":"..."}`
  - Gateway trả ACK rỗng downlink.
- **PASS**: data JSON khớp + ACK rỗng gửi về.
- ✅ Verify: script nhận data.

### TC-B02 — Compact 0x06 — merge giữ giá trị cũ khi field vắng
- Loại: IT | Tầng: Node↔GW
- **Tiền điều kiện**: Node đã gửi baseline `0x01` đầy đủ lần đầu (sau boot), nên gateway **đã có** soil/temp/hum/battery trong bảng node.
- Input (compact chỉ thay đổi soil(0x28=40) + flags(0x08), presence=0x04|0x10=0x14):
  `01 06 14 28 08 33` (payload: soil=0x28, flags=0x08; CRC=XOR(01^06^14^28^08)=0x33)
- Output: data JSON `soil=40` (đổi), nhưng `temp`/`hum`/`battery` **giữ giá trị cũ** (28,65,85). Node đánh dấu `ONLINE`.
- **PASS (đơn vị khẳng định logic)**: field **vắng** trong presence **không** bị ghi đè — gateway giữ nguyên giá trị đang có (`n->temp`, `n->hum`, `n->battery`).
- Verify: script nhận soil=40,temp=28,hum=65,bat=85.

### TC-B03 — Compact merge khi **gateway reboot + node chỉ gửi gói lẻ** → field vắng = 0
- Loại: IT | Tầng: Node↔GW (bổ sung cho lỗi bạn đang gặp)
- **Tiền điều kiện**: Node đã chạy ổn định (gửi delta/compact, KHÔNG gửi baseline 0x01 nữa). **Gateway vừa reboot** → bảng node trống.
- Bước: Node tiếp tục gửi compact khi có thay đổi, vd soil đổi:
  `01 06 04 28 2B` (presence=0x04 chỉ có soil; CRC=XOR(01^06^04^28)=0x2B)
- Output mong đợi (theo code `process_node` compact):
  - `node_find_or_create` tạo node mới với `memset` → `soil/temp/hum/battery=0`.
  - Merge: `soil=40` (từ packet), nhưng `temp=0, hum=0, battery=0` (không có baseline).
  - Gateway publish data: `{"node":1,"soil":40,"temp":0,"hum":0,"pump":0,"battery":0,...}`
- **PASS mong đợi (PASS khi hành vi đúng code)**: xác nhận data JSON có `temp:0,hum:0,battery:0`.
- **Ghi chú**: đây là hành vi hiện tại. Nếu muốn dữ liệu đúng, server nên gửi lệnh `{"cmd":"report"}` để node gửi gói `0x01` đầy đủ, hoặc node nên gửi baseline sau khi gateway reboot.

### TC-B05 — Heartbeat 0x02 — online + last_seen, KHÔNG publish data
- Loại: IT | Tầng: Node↔GW | Input: `01 02 00 00 00 00 55 B8` (battery=0x55).
- Output: gateway `node_mark_online` + update last_seen; **không publish `data`**. Nếu node vừa offline → publish status `online`. Trả ACK rỗng để reset gatewayLostCount.
- **PASS**: không có data mới; status chỉ publish khi node trước đó offline.
- ✅ Verify: gw log + script không nhận data mới.

### TC-B06 — Alarm 0x04 — cross-verify flags vs soil rồi publish alarm
- Loại: IT | Tầng: Node↔GW | Input: code sensor error (code=1) trong cả flags & soil:
  `01 04 01 01 00 00 FF FA` (battery=0xFF; crc=XOR(01^04^01^01^00^00^FF)=0xFA)
- Output mong đợi:
  - `.../node_01/alarm`: `{"node":1,"alarm_code":1,"type":"alarm","message":"Sensor error — 3 consecutive sensor read failures","flags":1}`
  - `node_mark_online`, last_seen update, **không ghi đè soil/temp/hum/battery**.
  - ACK rỗng gửi về node (node không ACK lại alarm).
- **PASS**: alarm JSON đúng + data không bị ghi đè.

### TC-B07 — Alarm mismatch flags vs soil — vẫn dùng flags
- Input: `01 04 03 05 00 00 FF <crc>` (flags=0x03 relay error, soil=0x05 mismatch).
- Output: gw log `Alarm code mismatch: flags=0x03 soil=0x05, using flags`; publish `alarm_code=3`.
- **PASS**: alarm_code=3 trong MQTT.

---

## NHÓM C — Gateway → Server (MQTT)

### TC-C1 — Schema data JSON chuẩn + threshold_exceeded=false
- Input: uplink 0x01 với soil=50 (trong band 30–70).
- Output: data JSON đủ `node,soil,temp,hum,pump,battery,timestamp,threshold_exceeded`; `threshold_exceeded=false`, **không** có `threshold_low/high`.
- `verify`: script `--verify` (data schema).

### TC-C02 — Vượt threshold — data có flag + alarm 3b
- Input: soil=15 (ngoài low=30).
- Output:
  - data: `"threshold_exceeded":true`, kèm `threshold_low=30,threshold_high=70`.
  - `.../node_01/alarm`: `{"node":1,"type":"alarm","message":"Sensor alarm triggered","soil":15,"threshold_low":30,"threshold_high":70}`
- `Verify`: script suite C.

### TC-C3 — Node offline sau timeout 180 + status
- Tiền: node từng online rồi dừng gửi.
- Mong đợi: sau **180s** timeout (check 1s) gateway publish `{"node":1,"status":"offline","timestamp":<epoch>}`.
- **PASS**: xuất hiện 1 message offline trong (180s,190s).

### TC-C04 — Gateway status — nodes_registered & commands_cached
- Điều kiện: 2 node đăng ký, 2 lệnh đang cache (node offline giả).
- Đọc `.../gw_01/status`: `nodes_registered: 2`, `commands_cached: 2`.
- Verify: khớp `command_cache_print` trên gw.

### TC-C5 — ALARM Mapper — code → message
- Input: từng code 1..5 (flags=soil=code).
- Output: `message` đúng bảng alarm trong mục Alarm codes.
- **PASS**: alarm JSON message đúng từng code.

---

## NHÓM D — Server → Node (lệnh, end-to-end)

> ⚠️ **Logic gửi lệnh MỚI (cache-first):** Mọi lệnh từ server được gateway **cache trước**, chỉ thực sự gửi xuống node **khi node gửi uplink** (data/heartbeat/alarm) — lúc đó `ack_or_flush_node()` flush lệnh xuống trong cửa sổ listen 350ms của node. Gateway **không** gửi lệnh ngay khi node `online` như trước. Vì vậy khi test: sau khi publish lệnh, hãy **đợi node báo lên (1 data/heartbeat)** mới thấy lệnh được chuyển; nếu node đang ngủ, lệnh nằm cache đến chu kỳ thức kế tiếp.

### TC-D01 — set_threshold (20–80) end-to-end
- Tiền: node 0x01 online, ngưỡng mặc định 30–70.
- Bước 1 (Server → GW): publish `.../node_01/cmd` = `{"cmd":"set_threshold","low":20,"high":80}`
- Bước 2 → GW: downlink **Input LoRa**: `01 05 04 14 50 44` (param1=0x14=20, param2=0x50=80, CRC=01^05^04^14^50=0x44)
- Bước 3 → node: `config_set_thresholds(20,80)` + NVS + ACK 0x03.
- Output: node log `Cmd: Set thresholds Low=20% High=80%`; gw log `ACK received`; **gw cập nhật local ngay** (`node_set_threshold`) trước khi ACK.
- **PASS**: (1) downlink đúng byte, (2) node ACK trong 350ms, (3) lệnh xoá khỏi cache, (4) local 20–80.
- ✅ Verify: serial node + gw; suite tự động.

### TC-D02 — set_interval (300s)
- `{"cmd":"set_interval","value":300}` → downlink `01 05 01 2C 01 28` (param1=0x2C, param2=0x01 → uint16=300; CRC=XOR(01^05^01^2C^01)=0x28).
- Node: `Cmd: Set sleep interval = 300 s` (clamp ≥5), save.
- Verify: chu kỳ deep sleep của node đổi (log cycle interval).

### TC-D03 — `on` với duration (60)
- Input: `{"cmd":"on","duration":60}`.
- Downlink: `01 05 02 3C 00 <crc>` (CMD_ON, param1=60, param2=0).
- Node: `pump_on` + `pumpBySchedule=true; scheduleDuration=60; interval=60`.
- Output: data kế `pump:1`; interval tạm=60.
- **PASS**: data `"pump":1` + node wake lại sau 60s tắt.

### TC-D04 — off
- Input: `{"cmd":"off"}` → downlink `01 05 03 00 00 <crc>`.
- Node: pump_off, `pumpBySchedule=false`.
- **PASS**: next data `"pump":0`.

### TC-D05 — report — ép gửi data ngay
- Input: `{"cmd":"report"}`.
- Node: `CMD_REQUEST_REPORT`, không dedup, gửi data 0x01 ngay.
- **PASS**: script nhận data mới trong vài giây (không cần delta).

### TC-D06 — toggle + idempotent dedup (trùng → ACK không tái thực)
- Input: `{"cmd":"toggle"}` x2 giống hệt.
- Node: lần1 toggle + ACK + store dedup; lần2 duplicate → **ACK nhưng không toggle lại**.
- **PASS**: pump đổi 1 lần; node log `Duplicate command ... ACKing without re-execution`.

### TC-D07 — set_mode
- `{"cmd":"set_mode","mode":1}` → downlink `01 05 08 01 00 <crc>`.
- Node: `MODE_SCHEDULE`. **PASS**: `node` log `Set mode = 1` (mode ≤2).

### TC-D08 — set_delta (type,value) + local cache
- `{"cmd":"set_delta","type":2,"value":5}` (soil delta=5).
- Downlink (6B): `01 05 0A 02 05 <crc>` (CMD_SET_DELTA, type param1=2, value param2=5).
- Gateway local: `node_set_delta_threshold(node, 2, 5)` cập nhật. Node: `config_set_delta_soil(5)` + NVS.
- **PASS**: log cả 2.

### TC-D09 — Lệnh từ server → cache → flush khi node gửi uplink kế tiếp
- **Đây là hành vi MẶC ĐỊNH** với logic cache-first mới — không chỉ khi node offline.
- Bước 1: node đang ngủ (không gửi uplink nào). Publish `{"cmd":"off"}` node 0x01.
- Bước 2: gateway **cache lệnh** (không gửi ngay); `commands_cached` tăng; **không có TX LoRa nào** xuất hiện trên serial.
- Bước 3: node thức dậy gửi data/heartbeat/alarm → gateway `ack_or_flush_node` → flush lệnh xuống trong cửa sổ listen 350ms.
- Output: `commands_cached` giảm sau flush; node log `Cmd: Relay OFF`; gateway log `ACK received` khi node ACK.
- **PASS**: (1) lệnh chỉ phát khi node thật sự thức (uplink), (2) node nhận + ACK.

### TC-D10 — ACK timeout 3s → lệnh trở lại "ready"; re-flush ở uplink kế; max 5 rồi drop
- Tiền: lệnh đã được flush xuống node nhưng **ACK bị mất** (tạm ngắt radio / node quá xa).
- Bước 1: `command_cache_process_timeouts()` sau 3s (`ACK_TIMEOUT_MS`) thấy `waiting_ack` hết hạn → `advance_retry` (retry_count++), tắt cờ `waiting_ack`.
- Bước 2: node gửi uplink kế tiếp → flush gửi lại lệnh.
- Bước 3: nếu retry_count đạt `MAX_RETRY` (5) → **drop** (pending=false), không gửi nữa.
- **PASS**: gw log `ACK timeout ... retrying`; sau 5 lần `exceeded max retries ... dropping`.
- Note: không có chuỗi backoff 1/2/4/8/16s chủ động từ task nữa — retry được điều khiển bởi thời điểm node thức, backoff chỉ áp dụng cho khoảng cách giữa các lần re-flush.

---

## NHÓM E — Node behaviors (hành vi node)

### TC-E01 — Threshold mode (2) — pump ON khi soil < low
- Điều kiện: mode 2, soil 25<low30. → `MODE_THRESHOLD` `pump_on`. Log `Moisture below threshold low. Turning pump ON.`. data next pump:1.

### TC-E02 — Threshold mode → pump OFF khi soil > high
- soil 75 > high70 khi đang tưới (`irrigation_active`) → `pump_off()`. Data next `pump:0`.

### TC-E03 — Schedule mode (1) + schedule duration restore
- mode=1, schedule 06:00, duration 60, interval bình thường 300s.
- Đến giờ: pump_on, `pump_by_schedule=true`, `interval=60`, `lastWateringDay`, hết 60s → pump_off, `interval` restore 300s. Chỉ 1 lần/ngày.
- **PASS**: interval tạm 60 → restore; pump vào giờ schedule.

### TC-E04 — Delta reporting compact
- soil 50→55 (delta soil 5) → gửi compact soil.
- **PASS**: 1 data thay đổi khi delta vượt.

### TC-E05 — Gateway lost → alarm 0x05 + fallback Schedule + hồi
- Điều kiện: gateway không ACK (heartbeat không downlink).
- Sau ≥3 heartbeat không downlink → `gatewayLostCount>=3` → alarm 0x05 + GW flag + `irrigation_gateway_lost()` → mode=Schedule.
- Output: `.../node_01/alarm` alarm_code=5; `FLAG_GATEWAY_LOST` (bit2) trong uplink; mode chuyển Schedule.
- Hồi phục: gw gửi ACK rỗng trở lại → `gatewayLostCount=0`, `gatewayLost=false`; node trở về mode cấu hình.
- **PASS**: alarm 0x05 nhận + node chuyển mode + phục hồi.

### TC-E06 — Alarm sensor error (3 lần đọc fail)
- 3 lần DHT/soil lỗi → `ALARM_SENSOR_ERROR`. → node gửi alarm packet 0x01; data không có `sensor_ok` (bit3).
- **PASS**: alarm JSON code=1.

---

## NHÓM F — System test (end-to-end)

### TC-F01 — Round-trip `toggle` (Server→Node→Server)
- publish `{"cmd":"toggle"}` → server thấy data mới có `pump` đổi.
- **PASS**: full chain trong <5s.

### TC-F02 — Threshold E2E: set_threshold → node exceed → data+alarm
- set_threshold(70,90); sau node vượt (soil=95 giả) → node `threshold` → data JSON `threshold_exceeded:true` + alarm `Sensor alarm triggered` với 70/90.
- **PASS**: 2 message (data+alarm) report.

### TC-F03 — Alarm propagation (node→server)
- Kích hoạt soil out-range (soil<10) → node alarm 0x02 → gw → server alarm JSON. **PASS**: message đúng.

### TC-F04 — Node mất điện khi có lệnh cache — flush sau online
- Node offline, cache 2 lệnh (off + toggle). Bật node → gateway flush, node ACK từng cái.
- **PASS**: 2 lệnh nhận + ACK; nếu toggle giống → dedup.

### TC-F05 — Gateway restart → cache RAM mất
- **Note**: cache là RAM-only (không NVS) → sau reboot `commands_cached` về 0, cache MẤT. Server phải gửi lại.
- Điều kiện PASS: sau reboot `commands_cached=0`; thi lại lệnh.

### TC-F06 — Broker outage + auto-reconnect
- Ngắt broker 60s, khôi phục.
- Dòng: `MQTT_DISCONNECTED` → `s_connected=false`; **LoRa vẫn nhận uplink** (node không GW_lost vì ACK rỗng vẫn gửi qua LoRa). Sau reconnect: re-subscribe cmd + publish online.
- **PASS**: node không bị "GW lost"; khi phục hồi lệnh server mới dùng được.

### TC-F07 — Node + Gateway cùng mất kết nối, rồi hồi phục
- Kết hợp TC-F06 + TC-E05, chờ status offline ở TC-C3. Sau khi cả 2 về → trạng thái `online` + có data mới.

---

## NHÓM G — Negative / Edge cases

### TC-G1 — Uplink CRC sai → ignore
- Input: `01 01 00 32 44 41 55 00` (CRC 0, sai).
- Output: `lora_read_packet` false. **PASS**: không data, không ACK.

### TC-G2 — Downlink sai node → no ACK
- node id=1, downlink dest=0x02 CRC ok. → `Downlink for node 0x02 - ignoring, no ACK`. **PASS**: không ACK 0x03.

### TC-G3 — JSON invalid / thiếu `cmd`
- Input: `{"foo":1}` hoặc không-JSON. Gw log `JSON command missing 'cmd'`; không forward, không cache. **PASS**: commands_cached 0.

### TC-G4 — command lạ
- `{"cmd":"do_thing"}` → `Unknown command`, không forward. **PASS**.

### TC-G5 — set_threshold low >= high → local reject
- `{"cmd":"set_threshold","low":80,"high":80}`.
- Gw: clamp low/high≤100, `low>=high` → local reject (local ngưỡng không đổi).
- **PASS**: local không đổi.

### TC-G6 — Node table đầy (11 node) → từ chối thêm
- Packet node mới đến khi đủ 10 → `Node table full, cannot add node`. không thêm.

### TC-G7 — Compact presence=0 (không có field thay đổi)
- Input: `01 06 00 07` (presence=0, CRC=XOR(01^06^00)=0x07).
- Output: `payload_count=0` → không field nào được merge; data publish = giá trị cũ; không crash.
- **PASS** (không đổi dữ liệu server): data JSON khớp baseline, không có field mới.

### TC-G8 — Battery=0xFF (external power)
- data `battery=0xFF`. Gw publish `battery:255`. Không alarm low-battery.

---

## Phụ lục — Cách verify / kết quả mong đợi

| Loại | Cách | Tool |
|---|---|---|
| byte-level LoRa | serial monitor gateway + node | `idf.py monitor` |
| MQTT payload | script listen (`-l`) | `mqtt_server_sim.py` |
| MQTT suite + report | `--suite file.json` → PASS/FAIL | `mqtt_server_sim.py` |
| trạng thái | nodes_registered / commands_cached / uptime | script |

**CRC8 (XOR) tính vài mẫu:**

| Hex bytes (trước CRC) | CRC |
|---|---|
| `01 05 04 14 50` | `0x44` |
| `01 05 01 2C 01` | `0x28` |
| `01 05 02 3C 00` | `0x3A` |
| `01 01 00 32 44 41 55` | `0x62` |
| `01 06 14 28 08` | `0x33` |
| `01 06 04 28` | `0x2B` |
| `01 06 00` | `0x07` |
| `01 04 01 01 00 00 FF` | `0xFA` |

*(Tự kiểm tra bằng hàm `crc8_calculate` trong `gateway_irr/main/crc.c`.)*

> Nhiệt độ: luôn dùng **wire byte** (nhiệt độ + 40) khi khai báo input.