# Yêu cầu đồng bộ: Threshold-based reporting cho Node firmware

**Trạng thái:** Cần implement

---

## 1. Bối cảnh

Gateway (firmware C + Simulator Python) đã được thêm cơ chế threshold-based reporting:
- Gateway lưu `threshold_low` / `threshold_high` cho mỗi node
- Khi data từ node đến, gateway so sánh `soil` với ngưỡng
- Nếu vượt ngưỡng → set `threshold_exceeded = true` trong data payload + publish alarm

Node cần được đồng bộ để có thể **tự so sánh và chỉ gửi data khi vượt ngưỡng**, giảm tải LoRa traffic.

---

## 2. Thay đổi cần implement trên Node firmware

### 2.1 Node entry — thêm threshold fields

```c
// Trong node data structure
#define THRESHOLD_LOW_DEFAULT   30
#define THRESHOLD_HIGH_DEFAULT  70

typedef struct {
    // ...các field cũ...
    uint8_t threshold_low;      // Ngưỡng dưới (mặc định 30%)
    uint8_t threshold_high;     // Ngưỡng trên (mặc định 70%)
    bool    threshold_exceeded; // Đã vượt ngưỡng ở lần đọc cuối?
} node_state_t;
```

### 2.2 Nhận lệnh `set_threshold` từ LoRa

Khi nhận được LoRa command `0x04` (LORA_CMD_SET_THRESHOLD):
- param1 = threshold_low
- param2 = threshold_high
- Lưu vào NVS để giữ qua reboot
- Gửi ACK về gateway

```c
static void handle_command(uint8_t cmd, uint8_t p1, uint8_t p2) {
    switch (cmd) {
        // ...các lệnh cũ...
        case LORA_CMD_SET_THRESHOLD:
            node_set_threshold(p1, p2);
            break;
    }
}

void node_set_threshold(uint8_t low, uint8_t high) {
    if (low >= high) return;  // invalid
    threshold_low = low;
    threshold_high = high;
    // Lưu vào NVS
    save_threshold_to_nvs(low, high);
}
```

### 2.3 So sánh threshold trước khi gửi data

```c
bool node_check_threshold(uint8_t soil) {
    if (soil < threshold_low || soil > threshold_high) {
        threshold_exceeded = true;
        return true;  // Vượt ngưỡng → cần gửi data
    }
    threshold_exceeded = false;
    return false;     // Trong ngưỡng → có thể skip
}
```

### 2.4 Cơ chế gửi data có điều kiện

```c
void sensor_loop() {
    read_sensors();  // soil, temp, hum, battery

    // Luôn gửi nếu có lệnh report pending
    if (report_pending) {
        send_uplink();
        report_pending = false;
        return;
    }

    // Kiểm tra threshold — chỉ gửi nếu vượt ngưỡng
    if (node_check_threshold(soil)) {
        send_uplink();           // Gửi data + set threshold_exceeded = 1
    }
    // Nếu trong ngưỡng, vẫn gửi theo chu kỳ dài hơn (heartbeat)
    else if (is_heartbeat_time()) {
        send_uplink();           // Gửi data với threshold_exceeded = 0
    }
}
```

### 2.5 Cập nhật LoRa uplink packet

Thêm flag `threshold_exceeded` vào packet:

```c
// Byte 2 (flags) — thêm bit threshold
#define FLAG_THRESHOLD_EXCEEDED  0x02  // Bit 1

// Khi build uplink packet
uint8_t flags = 0;
if (pump_state) flags |= 0x01;
if (threshold_exceeded) flags |= FLAG_THRESHOLD_EXCEEDED;

// Gửi packet 8 byte: {node_id, type, flags, soil, temp, hum, battery, crc}
```

### 2.6 NVS storage

```c
#define NVS_THR_LOW_KEY  "thr_low"
#define NVS_THR_HIGH_KEY "thr_high"

void save_threshold_to_nvs(uint8_t low, uint8_t high) {
    nvs_handle_t handle;
    nvs_open("node", NVS_READWRITE, &handle);
    nvs_set_u8(handle, NVS_THR_LOW_KEY, low);
    nvs_set_u8(handle, NVS_THR_HIGH_KEY, high);
    nvs_commit(handle);
    nvs_close(handle);
}

void load_threshold_from_nvs() {
    nvs_handle_t handle;
    nvs_open("node", NVS_READONLY, &handle);
    nvs_get_u8(handle, NVS_THR_LOW_KEY, &threshold_low);
    nvs_get_u8(handle, NVS_THR_HIGH_KEY, &threshold_high);
    nvs_close(handle);
}
```

---

## 3. Heartbeat — giữ node online

Gateway timeout đã tăng lên **180 giây** (3 phút). Node cần gửi heartbeat mỗi **60 giây** để gateway biết node còn sống.

### 3.1 Gửi heartbeat packet

```c
#define PKT_TYPE_HEARTBEAT  0x02
#define HEARTBEAT_INTERVAL_MS 60000

static uint64_t last_heartbeat = 0;

void sensor_loop() {
    uint64_t now = millis();

    // Kiểm tra đến lúc heartbeat chưa
    if (now - last_heartbeat >= HEARTBEAT_INTERVAL_MS) {
        send_heartbeat_packet();
        last_heartbeat = now;
    }

    // Đọc sensor (bình thường)
    read_sensors();

    // Chỉ gửi data nếu vượt ngưỡng hoặc đến kỳ heartbeat data
    if (node_check_threshold(soil)) {
        send_data_packet();    // soil vượt ngưỡng → gửi ngay
    } else if (is_data_heartbeat_time()) {
        send_data_packet();    // Gửi data định kỳ (~5-10 phút)
    }
}

void send_heartbeat_packet() {
    uint8_t pkt[8] = {0};
    pkt[0] = NODE_ID;
    pkt[1] = PKT_TYPE_HEARTBEAT;  // 0x02
    pkt[2] = 0;                   // flags (none)
    pkt[3] = 0;                   // soil (unused)
    pkt[4] = 0;                   // temp (unused)
    pkt[5] = 0;                   // hum (unused)
    pkt[6] = battery;             // battery (vẫn gửi để gateway monitor)
    pkt[7] = crc8(pkt, 7);
    uart_send(pkt, 8);
}
```

### 3.2 Gateway xử lý heartbeat (đã implement)

Gateway nhận `PKT_TYPE_HEARTBEAT` → chỉ gọi `node_mark_online()` + update `last_seen_ms`.  
**Không publish MQTT gì cả.** Điều này giảm tải MQTT cho server.

---

## 4. Luồng dữ liệu sau khi có threshold + heartbeat

```
┌─────────┐    LoRa packet    ┌──────────┐   MQTT publish   ┌────────┐
│ Node     │ ────────────────► │ Gateway  │ ────────────────►│ Server │
│ (so sánh │   (nếu vượt      │ (kiểm tra│   (luôn publish) │ (phân  │
│  ngưỡng) │    ngưỡng)       │  lại)    │   + add flag     │ tích)  │
└─────────┘                   └──────────┘                  └────────┘
     ▲                             │                            │
     │    set_threshold (LoRa)      │                            │
     └─────────────────────────────┘◄──── update threshold ──────┘
                                   (MQTT cmd → gateway → LoRa)
```

### Kịch bản:

1. Server gửi `{"cmd":"set_threshold","low":30,"high":70}` → MQTT
2. Gateway nhận, forward LoRa command `0x04` + cache local
3. Node nhận, lưu vào NVS, ACK
4. Node đọc sensor: `soil=25` → vượt ngưỡng dưới 30 → gửi ngay
5. Gateway nhận, thấy `threshold_exceeded = true`, publish data + alarm lên MQTT
6. Server nhận alarm, phân tích, có thể điều chỉnh ngưỡng mới

---

## 5. Các file cần sửa

| File | Thay đổi |
|---|---|
| `main/main.c` | Thêm `node_check_threshold()` trong sensor loop + gửi heartbeat định kỳ |
| `main/node.h` (hoặc tương đương) | Thêm threshold fields + heartbeat interval + các hàm |
| `main/nvs_config.c` | Thêm load/save threshold từ NVS |
| `main/lora.c` (hoặc phần xử lý command) | Thêm case `LORA_CMD_SET_THRESHOLD` |

---

## 6. Lưu ý

- **Heartbeat packet (type 0x02)** là packet 8 byte ngắn, các field soil/temp/hum = 0, chỉ có battery
- **Timeout** đã tăng từ 30s lên **180s** (`NODE_TIMEOUT_MS`) để phù hợp với heartbeat 60s
- Threshold mặc định: low=30, high=70
- `threshold_exceeded flag` ở byte flags (bit 1) giúp gateway biết ngay cả khi chưa kịp parse nội dung
- Nên gửi data heartbeat (data thật, không phải heartbeat packet) mỗi ~5-10 phút để server có data cập nhật
