# Test — Server ↔ Gateway ↔ Node

Bộ công cụ test IT + System cho hệ thống irrigation. Đọc tài liệu test case chính trước:
[`gateway_irr/docs/TEST_CASES.md`](../gateway_irr/docs/TEST_CASES.md).

```
test/
├── mqtt_server_sim.py      # Script mô phỏng SERVER (paho-mqtt)
├── suites/                 # Kịch bản suite (JSON) cho chế độ --suite
│   ├── bootstrap.json
│   ├── set_threshold.json
│   ├── toggle_roundtrip.json
│   ├── commands.json
│   ├── threshold_alarm.json
│   └── offline_status.json
├── reports/                # Báo cáo PASS/FAIL tự động sinh
└── README.md
```

---

## Cài đặt

```bash
cd test
python -m venv .venv
.\.venv\Scripts\activate        # Windows
# hoặc: source .venv/bin/activate  (Linux/macOS)

pip install paho-mqtt certifi
```

---

## Cách chạy

Tất cả lệnh đều cần thông tin broker. Có thể đặt trong env hoặc truyền qua CLI:

```bash
BROKER=mqtts://d246c46a2ebe40d2ae0c787f92bfdbab.s1.eu.hivemq.cloud
PORT=8883
USER=hivemq.webclient.1742180699133
PASS=<password từ config.c>
```

### 1. Listen mode (mặc định) — mô phỏng server tương tác

```bash
python mqtt_server_sim.py --broker $BROKER --port $PORT --user $USER --pass $PASS \
  --site HCM --gw gw_01 --nodes 1,2
```

- In mọi message `irrigation/HCM/gw_01/#` (topic + payload + timestamp).
- Bảng trạng thái node tự render mỗi 15s.
- **Gửi lệnh thủ công** (nhập trong terminal):
  ```
  > 1 {"cmd":"toggle"}
  > 2 {"cmd":"set_threshold","low":20,"high":80}
  ```

### 2. Suite mode — tự động verify + báo cáo

```bash
python mqtt_server_sim.py --broker $BROKER --port $PORT --user $USER --pass $PASS \
  --site HCM --gw gw_01 --suite suites/set_threshold.json
```

- Publish lệnh, chờ + validate payload nhận được, in **PASS/FAIL** từng bước.
- Báo cáo ghi vào `reports/<run_id>_<suite>.json` + `.md`.

### 3. Check-schema — validate 1 message nhận được

```bash
python mqtt_server_sim.py --broker $BROKER --port $PORT --user $USER --pass $PASS \
  --site HCM --gw gw_01 --check-schema data --timeout 30
```

- Chờ 1 message loại `data|status|alarm|gw_status` rồi validate theo
  `MAPPING_GIAO_THUC_MQTT.md`.

---

## Map test case → lệnh chạy

| Nhóm test | Tự động hóa (script) | Thủ công (serial) |
|---|---|---|
| A — Bootstrap | `--suite suites/bootstrap.json` | boot node (baseline 0x01), gw online publish |
| B — Node→GW (LoRa uplink) | nhận data ở bất kỳ mode nào | quan sát serial node/gw, byte gói |
| C — GW→Server | `--suite suites/bootstrap.json`, `--check-schema data`, `--suite suites/threshold_alarm.json` | node offline 180s → `offline_status.json` |
| D — Server→Node | `--suite suites/commands.json`, `set_threshold.json`, `toggle_roundtrip.json` | xác nhận byte downlink trên serial |
| E — Node behaviors | — | serial node (mode, schedule, delta, GW lost) |
| F — System | `toggle_roundtrip.json`, `threshold_alarm.json` | outage/recovery kịch bản tay |
| G — Negative/edge | — | inject byte/JSON lỗi + quan sát |

---

## Lưu ý

- **Broker mặc định** trong `config.c` là HiveMQ Cloud (TLS 8883). Nếu dùng broker khác
  (ví dụ local Mosquitto), thêm `--insecure` khi broker không có CA chuẩn.
- `reports/` tự tạo khi chạy suite; thêm vào `.gitignore` nếu không muốn commit.
- Tham số `--nodes` chỉ để hiển thị; trạng thái node thật do gateway report qua MQTT.

## Liên hệ

Bộ test này dùng để verify firmware `gateway_irr` + `node_irr`. Khi sửa protocol,
cập nhật đồng thời: `MAPPING_GIAO_THUC_MQTT.md`, `TEST_CASES.md`, và `SCHEMAS` trong `mqtt_server_sim.py`.
