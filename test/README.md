# Test — Server ↔ Gateway ↔ Node

Bộ công cụ test IT + System cho hệ thống irrigation. Đọc tài liệu test case chính trước:
[`gateway_irr/docs/TEST_CASES.md`](../gateway_irr/docs/TEST_CASES.md).

```
test/
├── mqtt_server_sim.py      # Script mô phỏng SERVER (paho-mqtt)
├── BASELINE_TEST_CASES.md  # Testcase baseline/slot/boot-sync (bổ sung TEST_CASES.md)
├── suites/                 # Kịch bản suite (JSON) cho chế độ --suite
│   ├── bootstrap.json
│   ├── set_threshold.json
│   ├── toggle_roundtrip.json
│   ├── commands.json
│   ├── threshold_alarm.json
│   ├── offline_status.json
│   ├── baseline_provision.json   # cấp baseline 3 series (schema CŨ) + done
│   ├── baseline_provision_new_schema.json  # schema MỚI: 1 message = 3 series
│   ├── baseline_sequential_2nodes.json     # TC-I12: cấp baseline TUẦN TỰ 2 node
│   ├── baseline_relay_sequential.json      # TC-I12b: server CHỨ push bảng node sau khi GW relay request
│   └── slot_sync.json            # sync_slot + regression
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
| **H — Baseline / slot / boot-sync** | `--suite suites/baseline_provision.json`, `baseline_provision_new_schema.json`, `baseline_sequential_2nodes.json`, `baseline_relay_sequential.json`, `slot_sync.json`, `--check-schema baseline` | serial 2 phía — xem [`BASELINE_TEST_CASES.md`](BASELINE_TEST_CASES.md) |

> **Suite engine:** message KHÔNG khớp bước đang chờ được **đệm lại (pending buffer)** thay vì vứt đi — bắt buộc khi nhiều node chạy song song (request của node 2 có thể vượt node 1).
> **Ctrl+C giữa suite:** dừng gọn — bước đang chờ được đánh dấu `INTERRUPTED`, các bước đã chạy vẫn được ghi vào báo cáo, MQTT được đóng an toàn (không còn traceback kép từ `loop_stop()`); exit code 130.
> Action mới `assert_not_seen` (`topic_regex` + `expect`, tuỳ chọn `settle_seconds`) để khẳng định một message **KHÔNG được** xuất hiện — dùng kiểm tra thứ tự trong TC-I12.

> **Tự phục hồi khi gateway mất bảng baseline:** session baseline của gateway chỉ nằm trong **RAM**, nên nếu gateway reset giữa lúc gửi chunk (log `rst:0x1 (POWERON_RESET)` ngay sau `TX: waiting AUX low`) thì bảng mất và node sẽ `REQ` mãi mà không bao giờ `done`. Gateway phát hiện và publish `request` kèm **`"needed": 1`**; runner lập tức **push lại đúng message `set_baseline` đã cache** cho node đó (tối đa 5 lần/node) → suite tự chạy tiếp, không cần thao tác tay.
> ⚠️ Về phần cứng: reset lúc TX là dấu hiệu **nguồn không đủ / sụt áp khi LoRa phát** (không có panic/backtrace trong log). Hãy dùng nguồn 5V chắc chắn, thêm tụ ~470 µF sát chân VCC module LoRa, và **không** cấp nguồn lại gateway trong lúc nó đang truyền baseline.

> **Gateway là bộ đệm/relay (TC-I12b):** node thiếu baseline → gateway chuyển request lên server (thêm `"needed": 1` nếu gateway đang không giữ bảng). Nếu node khác cũng REQ trong lúc node hiện tại đang được cấp, gateway **xếp hàng đợi và CHƯA báo server**; chỉ khi node đang phục vụ `done` đủ serie thì request kế tiếp mới được chuyển lên server (log `Relaying queued baseline request of node 0xXX ... to server`). Khi xong, gateway **xoá bảng của node** (`baseline session COMPLETE - table dropped`) và gửi server **ĐÚNG MỘT message `done`** `{"node":N,"status":"done","version":v,"series_mask":m}`.
> Node báo xong bằng 1 frame `0x08` mang **bitmask** (tránh mất frame giữa chuỗi), và sau khi cấp baseline xong **luôn gửi thêm 1 REQ time** để chốt `t`. Suite kiểm chứng: `suites/baseline_relay_sequential.json`.

> **Mã TC:** các suite dùng tên gọi theo tài liệu cũ (`TC-BPxx`, `TC-BLxx`…). Bảng đối chiếu
> sang mã **A–K** của `Test_Case_Tuoi_Tu_Dong.xlsx` nằm ở sheet **“Map Suite - TC”** trong file Excel đó.

### Chạy nhanh nhóm H (baseline)

```bash
# 1. Cấp baseline cho node 0x01 (node phải có NVS baseline TRỐNG)
python mqtt_server_sim.py --broker $BROKER --port $PORT --user $USER --pass $PASS \
  --site HCM --gw gw_01 --nodes 1,2 --suite suites/baseline_provision.json

# 2. Sync slot + regression frame 7 byte
python mqtt_server_sim.py --broker $BROKER --port $PORT --user $USER --pass $PASS \
  --site HCM --gw gw_01 --nodes 1,2 --suite suites/slot_sync.json

# 3. Validate payload topic baseline
python mqtt_server_sim.py --broker $BROKER --port $PORT --user $USER --pass $PASS \
  --site HCM --gw gw_01 --check-schema baseline --timeout 600
```

> ⚠️ Xoá baseline trong NVS node để test luồng provisioning: `idf.py -p COMx erase-flash` rồi flash lại.
> Ép node gửi uplink (để gateway có cớ đẩy downlink): nhấn nút GPIO34 trên node.

---

## Lưu ý

- **Broker mặc định** trong `config.c` là HiveMQ Cloud (TLS 8883). Nếu dùng broker khác
  (ví dụ local Mosquitto), thêm `--insecure` khi broker không có CA chuẩn.
- `reports/` tự tạo khi chạy suite; thêm vào `.gitignore` nếu không muốn commit.
- Tham số `--nodes` chỉ để hiển thị; trạng thái node thật do gateway report qua MQTT.

## Liên hệ

Bộ test này dùng để verify firmware `gateway_irr` + `node_irr`. Khi sửa protocol,
cập nhật đồng thời: `MAPPING_GIAO_THUC_MQTT.md`, `TEST_CASES.md`, và `SCHEMAS` trong `mqtt_server_sim.py`.
