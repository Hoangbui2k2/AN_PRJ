# Plan: Baseline nội suy trên Node (phương án 1 — chunked)

> **Trạng thái:** ĐÃ TRIỂN KHAI (Phase 1–5) — chờ build + test
> **Ngày:** 2026-09-13
> **Phạm vi:** `node_irr/` (node) + `gateway_irr/` (gateway)

---

## 0. Trạng thái triển khai

| Phase | Nội dung | Trạng thái |
|---|---|---|
| 1 | Gateway slot manager + broadcast biên | ✅ Xong (`slot.c/h`, `main.c`) |
| 2 | Protocol 7 byte + type `0x07/0x08/0x09` | ✅ Xong (cả node + gateway) |
| 3 | Gateway `baseline_manager` + MQTT `set_baseline` / `baseline` | ✅ Xong |
| 4 | Node `baseline.c/h`: reassembly, NVS, nội suy | ✅ Xong |
| 4b | Node boot/reset sync (blocking + safety cap) | ✅ Xong (`node_boot_sync`) |
| 4c | Node thức sát 00:00 + nhận broadcast | ✅ Xong (`node_park_near_boundary`) |
| 5 | Node chu kỳ cố định 300 s + tích hợp send-decision | ✅ Xong |
| 6 | Build + verification | ⏳ Chờ chạy |

**Việc còn lại:** build cả 2 project, và test theo mục 8 bên dưới.

---

## 1. Yêu cầu đã chốt

- Điểm nền = `(t, y)`: `t ∈ [0,95]` (1 byte), `y` = 1 byte (giá trị temp/hum/soil).
- 3 series riêng: `series_id` 0 = temp, 1 = hum, 2 = soil. Max **96 điểm/node/series** (1 điểm = 1 slot). **Server gửi 1 bảng tin**, gateway tách thành 3 series.
- `t` do **gateway gửi kèm MỖI downlink**. Server sync `t` về 0 (broadcast).
- Mục đích nội suy: **quyết định CÓ GỬI uplink hay không** (tiết kiệm pin). **CÓ** điều khiển tưới khi ở chế độ tự động theo threshold.
- Danh sách riêng từng node; gửi tuần tự từng node; node nhận xong → gateway báo server → server gửi node kế tiếp.
- Ngưỡng gửi sau nội suy: **temp = 1** (1 °C), **hum = 2** (%), **soil = 2** (%). **KHÔNG** tái dùng delta.
- Chu kỳ ngủ node cố định **300 s (5 phút)**.
- NVS đã có baseline → không tải lại; **NHƯNG** vẫn phải **sync `t`** nếu node bị reset (mất RTC).

---

## 2. Đặc tả cứng trục thời gian `t`

- `t ∈ [0,95]` → **96 slot/ngày**, mỗi slot **CỐ ĐỊNH 15 phút**.
  - `SLOTS_PER_DAY = 96`, `SLOT_MINUTES = 15`, `SLOT_MS = 900000`.
  - 96 × 15 = 1440 phút = 24h (khớp trọn ngày).
- Slot KHÔNG phụ thuộc số điểm nền: baseline tối đa 96 điểm (1 điểm/slot) là **tập mẫu thưa** trên 96 slot; slot thiếu do nội suy.
- Gateway tính `slot = ((now_ms − sync_base_ms) / SLOT_MS) % 96`, gửi kèm mỗi downlink.
- Node nội suy **CẢ 3 series** tại slot hiện tại; gửi nếu **bất kỳ** series nào lệch ≥ tolerance.
- Biên: `t=0` và `t=95` **kẹp biên** (clamp), không ngoại suy.
- Ví dụ: 8h sáng → slot **32** (8 × 4).

---

## 3. Chu kỳ ngủ cố định = 5 phút

- Node deep-sleep **cố định 300 s**, không đổi runtime.
- ⚠️ **Bug hiện tại:** `node_irr/main/config.c` → `config_init_default()` đặt `interval = 10` nhưng comment ghi `/* 5 minutes */` → sửa thành `300`. `normalInterval` đã là 300.
- **Quan hệ slot:** 300 s × 3 = 900 s → **đúng 3 chu kỳ thức / 1 slot**.
- **Gateway phải khớp:** `gateway_irr/main/node_manager.h` → `REPORT_INTERVAL_DEFAULT 10` → **300**.
  - Nếu không: `node_timeout_ms()` = 10 × 6 × 2 = **120 s** → node bị đánh dấu offline liên tục.
  - Với 300 s: timeout = 300 × 6 × 2 = **3600 s (60 phút)** — hợp lý cho heartbeat mỗi 25 phút (`HEARTBEAT_CYCLES = 5`).
- `HEARTBEAT_INTERVAL_MS 60000` (legacy) → rà soát để không dùng cho timeout.
- `CMD_SET_INTERVAL (0x01)`: node ACK nhưng **giữ 300 s**.
- **Lưu ý pin:** 288 lần thức/ngày → cần đo dòng trung bình.

---

## 4. Luồng đồng bộ lúc BOOT / RESET ⭐

**Hai nhu cầu ĐỘC LẬP:**

| Nhu cầu | Điều kiện | Nguồn chân lý |
|---|---|---|
| **Sync `t`** | `slot_valid == false` — reset làm mất RTC | Cờ RTC `slot_valid` |
| **Tải baseline** | `baseline_load_nvs()` thiếu / `version = 0` | NVS blob |

- Reset nguồn / brownout / watchdog / software reset → **RTC mất** → cần sync `t`.
- Deep-sleep wake → RTC giữ → chỉ chạy luồng nếu baseline NVS thiếu.
- **NVS đã có baseline** → KHÔNG tải lại baseline (chỉ có thể cần sync `t`).

**Điều kiện chạy luồng:** `(!slot_valid) || (!baseline_valid)`

- Cả hai OK → **BỎ QUA hoàn toàn**, chạy chu kỳ bình thường.
- Chỉ thiếu `slot` → chỉ xin time (1 lượt, rất nhanh).
- Thiếu baseline → tải baseline (dù có hay không có slot).

**Giao thức:** 1 uplink chung `REQ (0x09)` với `flags`:

- `bit0 REQ_BASELINE` — cần baseline (kèm `series_mask`).
- `bit1 REQ_TIME` — cần slot hiện tại.

**Hành vi:**

1. Node gửi `REQ` với flags phù hợp.
2. Mở cửa sổ nghe **30 s**.
3. Gateway:
   - `REQ_TIME` → gửi ngay 1 downlink (mọi frame mang `slot`) → node `config_set_current_slot()` + `slot_valid = true`.
   - `REQ_BASELINE` → publish MQTT request → server trả **1 `set_baseline`** (1 bảng tin = cả 3 series) → gateway tách thành 3 series → chunk `0x07` (mang kèm slot).
4. Node reassembly từng series → commit NVS → gửi `0x08 BASELINE_DONE`.
5. Chưa đủ → gửi lại `REQ`, lặp lại.

**Khác biệt quan trọng:** sync `t` chỉ cần **1 lượt** (rất nhanh); tải baseline cần nhiều frame.

Nếu gateway chết: node có thể chạy tạm với `slot_valid = false` → **tắt baseline gating** (gửi như bình thường) và retry sync ở chu kỳ sau. Baseline thì vẫn chặn theo quyết định blocking (có safety cap).

✅ **Safety cap (đã chốt):** sau **~15 phút** hoặc khi **pin < 20%** → ngủ tạm 5 phút rồi thử lại (không thức mãi).

### Ma trận quyết định

| Tình huống | RTC `slot` | NVS baseline | Hành động |
|---|---|---|---|
| Reset nguồn / brownout / watchdog / software reset | ❌ mất | ✅ có | **Chỉ xin time** (1 lượt) |
| Reset nguồn | ❌ mất | ❌ trống | Xin **cả** time + baseline |
| Deep-sleep wake | ✅ còn | ✅ có | **Bỏ qua hoàn toàn** |
| Deep-sleep wake, lần đầu chưa có baseline | ✅ còn | ❌ trống | Xin **baseline** (đã có slot) |
| Reset khi gateway chết | ❌ mất | ✅ có | Chạy tạm (base gating off), **retry sync chu kỳ sau** |

---

## 5. Thức sát 00:00 (chuẩn bị sync t=0)

- Node dùng **thời gian tích luỹ RTC** để biết sắp hết ngày (slot ~95).
- Ngủ thêm sao cho thức **~2–5 phút trước mốc t=0** (không thức từ t=94 suốt 30 phút).
- Thức hoàn toàn: LoRa Normal, mở **cửa sổ RX dài (~5 phút)** chờ broadcast.
- Gateway broadcast `dest=0xFF, cmd=0x09, slot=0` tại mốc biên, **lặp mỗi 30–60 s trong ~5 phút**.
- Nhận được → `slot = 0`, reset bộ đếm tích luỹ, ngủ lại bình thường.
- Không nhận được → giữ slot ước lượng, thử tiếp.
- Giảm **sync skew** giữa các node từ ~5 phút xuống mức cửa sổ nghe chung.
- Chi phí: ~5 phút thức/ngày.

---

## 6. Đánh giá tài nguyên

- **Gateway RAM:** 96 × 2B = 192 B/series; 576 B/node; 10 node = **~6 KB** → thừa sức (heap ESP32 còn ~100–180 KB).
- **KHÔNG** dùng `command_cache` (20 slot, dedup theo byte) → cần module `baseline_manager` riêng.
- Nút thắt thật là **airtime + số chu kỳ thức**, không phải RAM → gửi tuần tự từng node là đúng.
- **Node:** session dở dang phải sống qua deep sleep → `RTC_DATA_ATTR`; commit NVS khi xong.
- Ước lượng: chunk 10 điểm → **10 frame/series → 30 frame/node** (3 series); 3 chunk mỗi lần flush → ~10 chu kỳ thức (≈50 phút/node).

---

## 7. Quyết định thiết kế

1. Frame command mở rộng **6 → 7 byte**: `dest|0x05|cmd|p1|p2|slot|crc`. Mọi downlink mang `t`.
2. Frame mới `0x07 BASELINE_CHUNK` (GW→Node): `dest|0x07|series|version|seq|total|n_points|[t,y]×n|crc` (≤ 28 B).
3. Uplink mới `0x08 BASELINE_DONE`: node→GW khi xong 1 series.
4. MQTT topic `.../node_XX/baseline` cho 2 event: `request` và `done`.
5. Sync `t=0`: gateway **broadcast** `dest=0xFF|0x05|0x09|0|0|slot=0|crc` tại mốc biên (lặp 30–60 s × ~5 phút).
6. Baseline gating là điều kiện **OR** thêm vào Step 9 của `main.c` node; giữ nguyên delta/threshold/heartbeat.
7. Tolerance cố định: **temp = 1, hum = 2, soil = 2** — KHÔNG dùng delta.
8. Trục `t` cứng: 96 slot × 15 phút; hằng số ở `config.h` cả 2 phía.
9. Gửi nếu `|meas − interp(slot)| >= tol` cho bất kỳ series nào (series không có baseline thì bỏ qua).
10. Chu kỳ ngủ cố định 300 s; gateway `REPORT_INTERVAL_DEFAULT = 300`.
11. `CMD_SET_INTERVAL` vô hiệu hoá phía node.
12. **Server gửi 1 bảng tin cho cả 3 series**; gateway tách thành 3 series (`baseline_store()` × 3) rồi chunk `0x07` xuống node.
13. Tolerance không phải delta; temp dùng thang `offset + 40` (1 đơn vị = 1 °C) nên `tol = 1` khớp tự nhiên.
14. **Luồng đồng bộ boot/reset tách 2 nhu cầu:** sync `t` (cờ `slot_valid` RTC) và tải baseline (NVS).
15. Thức sát 00:00 rồi nhận broadcast (không thức suốt từ t=94).
16. Timeout mỗi lần thử = **30 s**.
17. Sync `t` chỉ cần **1 downlink** (mọi frame mang slot) — nhanh hơn nhiều so với tải baseline.
18. Uplink `0x09 REQ` với `flags` (bit0 baseline, bit1 time) + `series_mask`.
19. MQTT `/baseline` dùng chung 1 topic cho request + done.
20. NVS là nguồn chân lý cho baseline; cờ RTC `slot_valid` là nguồn chân lý cho time.
21. **Safety cap (chốt):** luồng tải baseline ngủ tạm sau ~15 phút hoặc khi pin < 20%, ngủ 5 phút rồi thử lại.
22. **"NVS hợp lệ" = blob + `version > 0` + đủ điểm; KHÔNG TTL** → không tự refresh định kỳ.
23. **Broadcast biên: lặp mỗi 30–60 s trong ~5 phút.**
24. **Max 96 điểm/series** (1 điểm = 1 slot); mảng thưa, nội suy lấp chỗ trống. `BASELINE_MAX_POINTS = 96` cả 2 phía.
25. **Push-only:** node KHÔNG định kỳ xin lại bảng dự đoán — server đẩy version mới thì gateway tự gửi xuống trong các cửa sổ downlink kế tiếp (node tự mở session khi thấy version mới). Chỉ `REQ` khi NVS trống/không hợp lệ (provisioning lần đầu).
26. **Cửa sổ downlink phải drain hết burst:** node đọc tới khi line idle (gap 150 ms thường / 400 ms giữa 2 chunk baseline, cap 3 s). Nếu chỉ đọc 1 frame thì phần còn lại của burst bị mất và series không bao giờ hoàn tất.

---

## 8. Phases

### Phase 1 — Gateway: slot manager + sync + broadcast biên

- Hằng số: `SLOTS_PER_DAY 96`, `SLOT_MINUTES 15`, `SLOT_MS 900000`.
- `slot_current()` = `((now_ms − sync_base_ms) / SLOT_MS) % 96`; `slot_sync_reset()`.
- `handle_mqtt_command()`: nhánh `sync_slot` (node 0xFF = broadcast).
- Khi slot vượt 95 → 0: broadcast `lora_send_command(0xFF, 0x09, 0, 0)` + `slot=0`, **lặp mỗi 30–60 s trong ~5 phút**.
- `node_manager.h`: `REPORT_INTERVAL_DEFAULT` 10 → **300**; rà soát `HEARTBEAT_INTERVAL_MS`.
- **Deps:** không.

### Phase 2 — Protocol mở rộng (gateway + node cùng lúc)

- GW `lora_uart.h/.c`: type `0x07`, `LORA_DOWNLINK_SIZE 7`, thêm `slot`; sửa `rx_is_known_type`/`rx_frame_len` (cmd = 7, `0x07` variable), discard echo `0x07`; `lora_send_command`/`lora_send_ack` build 7 byte; `lora_send_baseline_chunk()`.
- Node `config.h`: type `0x07`/`0x08`/`0x09 REQ`, `lora_cmd_packet_t` + `slot`, `SLOT_*`, `BASE_TOL_*`.
- Node `commands.c`: `lora_receive_frame` (need = 7 cho `0x05`, `0x07` theo `n_points`), lưu slot ở `wait_downlink`/`check_pending`, `commands_process` cập nhật slot.
- **Deps:** chặn Phase 3, 4, 4b, 4c. **Hai firmware phải nạp cùng nhau (breaking).**

### Phase 3 — Gateway: baseline_manager + MQTT (depends on Phase 2)

- Module mới `baseline_manager.c/h`: session per-node `{active, series, version, total, sent_seq, done}`, buffer 90 B/series.
- `handle_mqtt_command` nhánh `set_baseline` (parse JSON, validate `t` tăng dần 0–95, ≤ 96 điểm).
- `process_node_packet()` xử lý uplink `0x08` (done) và `0x09 REQ` (request) → publish `.../node_XX/baseline`.
- `ack_or_flush_node()` ưu tiên flush baseline session.
- `topic.c/h`: `TOPIC_NODE_BASELINE`. `CMakeLists.txt`: thêm `baseline_manager.c`.

### Phase 4 — Node: reassembly + NVS + nội suy (depends on Phase 2)

- Module mới `baseline.c/h`:
  - `baseline_session_t` (`RTC_DATA_ATTR`): active, series, version, total, received bitmap, `data[90]`.
  - `baseline_handle_chunk()`, `baseline_session_complete()`, `baseline_commit_nvs()`, `baseline_load_nvs()`.
  - `baseline_interp(series, slot)` — tìm đoạn bao quanh slot; kẹp biên; guard `dt<=0`; nhân `int64`; round half-away-from-zero.
  - `baseline_eval_all(slot, out[3])`.
  - `baseline_should_send(data, slot)` — `dev = |meas − interp|`; true nếu bất kỳ series nào `dev >= BASE_TOL_*` (1/2/2).
- `config.c`: NVS blob `bl_v0/bl_v1/bl_v2`; `slot_valid`, `slot_current` (RTC); `baseline_advance_slot(elapsed)`.
- `CMakeLists.txt`: thêm `baseline.c`.

### Phase 4b — Node: luồng đồng bộ boot/reset (depends on 2, 3, 4)

- Điều kiện: `!slot_valid || !baseline_valid` (NVS). Cả hai OK → return ngay.
- Nếu thiếu baseline: gửi `REQ` flags = baseline|time; vòng lặp nghe 30 s, retry tới khi đủ; **KHÔNG** `power_deep_sleep()` trong lúc chờ.
- Nếu chỉ thiếu time: gửi `REQ` flags = time; nhận 1 downlink → `slot_valid = true` là xong.
- Nếu gateway chết khi chỉ thiếu time: chạy tạm với `slot_valid = false` (tắt baseline gating), retry chu kỳ sau.
- Sau khi đủ: `baseline_commit_nvs()`, chốt slot, thoát chế độ boot.
- **Safety cap (chốt):** ~15 phút hoặc pin < 20% → ngủ tạm 5 phút rồi thử lại.

### Phase 4c — Node: thức sát 00:00 + nhận broadcast (depends on 1, 2)

- Tính thời gian còn lại tới mốc t=0 từ bộ đếm RTC; đặt timer ngủ tới ~2–5 phút trước mốc.
- Mở cửa sổ RX dài (~5 phút) chờ broadcast `dest=0xFF cmd=0x09 slot=0`.
- Nhận được → `slot=0`, `slot_valid=true`, reset bộ đếm. Không nhận được → giữ slot ước lượng.

### Phase 5 — Node: chu kỳ cố định + tích hợp send-decision + DONE (depends on 1, 3, 4)

- `config_init_default()`: `interval = 10` → **300** (sửa comment sai); giữ `normalInterval = 300`.
- `CMD_SET_INTERVAL`: ACK + log "interval fixed 300s", không đổi.
- Step 7/10: lưu `slot` từ downlink.
- Slot advance: cộng dồn thời gian RTC; 3 chu kỳ = 1 slot; không có downlink thì ước lượng.
- Step 9: `base = baseline_should_send(&sensor_data, slot)` OR vào chuỗi quyết định; log series lệch + mức lệch.
- Khi session complete: `baseline_commit_nvs()` + gửi `0x08`.

### Phase 6 — Verification

1. `idf.py build` cả 2 project.
2. Test nội suy: bảng biết trước, gồm `dy` âm (nhiệt độ giảm) và biên slot 0/95; 3 series cùng lúc.
3. Test trục `t`: 96 × 15 = 1440 phút; `slot_current()` quay vòng 96 → 0.
4. Integration qua `test/mqtt_server_sim.py`: `set_baseline` → GW chunk, node reassembly, NVS save, uplink `0x08`, MQTT done; ≥ 2 node tuần tự.
5. Chu kỳ 5 phút: node **KHÔNG** bị đánh offline (timeout 3600 s > heartbeat 25 min); 3 chu kỳ → slot +1.
6. Regression: các lệnh cũ chạy với frame 7 byte; `set_interval` bị từ chối; heartbeat + gateway-lost không đổi.
7. Deep sleep: slot tiến đúng; baseline sống qua cold boot (NVS).
8. **Đồng bộ boot/reset:**
   - (a) NVS trống → node KHÔNG ngủ, gửi REQ, giả lập server chậm → xác nhận retry.
   - (b) NVS **đã có** baseline + reset nguồn (RTC mất) → node **chỉ xin time**, không tải baseline, chạy bình thường.
   - (c) NVS có baseline + deep-sleep wake (RTC còn) → **bỏ qua hoàn toàn**.
   - (d) Reset khi gateway chết → node chạy tạm (base gating off) và retry sync chu kỳ sau.
   - (e) Gateway chết lâu khi đang tải baseline → xác nhận **safety cap** (~15 phút / pin < 20%) cho node ngủ tạm rồi thử lại.
9. **Thức sát 00:00:** mô phỏng gần biên → thức ~2–5 phút trước, nhận broadcast `slot=0` (**lặp 30–60 s trong ~5 phút**), reset đúng.
10. **Tolerance:** đo lệch đúng ngưỡng temp = 1 / hum = 2 / soil = 2 (bằng ngưỡng → phải gửi).
11. Đo thời gian/dòng trung bình (288 wake + provisioning + cửa sổ 00:00).

---

## 9. Relevant files

**Gateway (`gateway_irr/main/`)**

- `lora_uart.h` / `lora_uart.c` — frame types, downlink 7 byte, chunk send.
- `main.c` — `handle_mqtt_command`, `ack_or_flush_node`, `process_node_packet`.
- `baseline_manager.c` / `baseline_manager.h` — **NEW**.
- `topic.c` / `topic.h` — `TOPIC_NODE_BASELINE`.
- `node_manager.h` — `REPORT_INTERVAL_DEFAULT`.
- `CMakeLists.txt`.

**Node (`node_irr/main/`)**

- `config.h` / `config.c` — slot fields, `BASE_TOL_*`, NVS blob, interval.
- `commands.c` — `lora_receive_frame`, `wait_downlink`, `check_pending`, `process`, REQ.
- `baseline.c` / `baseline.h` — **NEW** (session + interp + should_send).
- `main.c` — boot sync flow, Step 7/9/10, 00:00 wake.
- `power.c` / `power.h` — reset reason / wake cause.
- `CMakeLists.txt`.

---

## 10. Các điểm mở — ĐÃ CHỐT HẾT

1. ✅ **Safety cap:** luồng tải baseline ngủ tạm sau **~15 phút** hoặc khi **pin < 20%**, ngủ 5 phút rồi thử lại.
2. ✅ **"NVS hợp lệ"** = có blob + `version > 0` + đủ số điểm. **KHÔNG có TTL/hết hạn** → không tự refresh định kỳ (server muốn cập nhật thì đẩy `set_baseline` hoặc vô hiệu hoá NVS).
3. ✅ **Slot khi không có downlink:** đếm chu kỳ (3 = +1 slot) làm chính, cộng dồn thời gian RTC dự phòng khi interval bị override.
4. ✅ **Broadcast biên:** lặp **mỗi 30–60 s trong ~5 phút**.
5. ℹ️ `t ∈ [0,95]` là **96 slot** (không phải 95). 96 × 15 phút = 24h tròn.

---

## 11. ⚠️ Lưu ý trước khi thực thi

1. **Breaking change:** frame command đổi 6 → 7 byte ⇒ **phải nạp cả gateway và node cùng lúc**, nếu không hai bên hiểu sai frame.
2. **Bug sẵn có cần sửa kèm:** `node_irr/main/config.c` → `interval = 10` (comment ghi "5 minutes" — sai).
3. **Thứ tự thực thi đề xuất:** Phase 2 trước (protocol), rồi Phase 1/3/4 có thể song song, sau đó 4b/4c/5, cuối cùng Phase 6.
