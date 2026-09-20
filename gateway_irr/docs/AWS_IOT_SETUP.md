# AWS IoT Core Setup — cho ESP32 Gateway

Hướng dẫn từ đầu tới cuối để đưa data từ gateway ESP32 lên **AWS IoT Core** qua MQTT
(xác thực bằng X.509 certificate / mutual-TLS). Dành cho người **chưa có gì** trên AWS.

---

## 1. Tổng quan luồng

```
Gateway ESP32  ──(mutual TLS, cert)──►  AWS IoT Core endpoint
                                          ├─ publish  irrigation/HCM/gw_01/.../data
                                          ├─ subscribe irrigation/HCM/gw_01/+/cmd
                                          └─ (tùy chọn) Rule → DynamoDB / S3 / Lambda / Timestream
```

AWS IoT Core **không** dùng username/password như HiveMQ. Mỗi device tự xác thực bằng
một **certificate** được cấp bởi AWS và gắn với một **IoT Policy**.

---

## 2. Chuẩn bị

- Tài khoản AWS (https://aws.amazon.com). Region đề xuất gần VN: **ap-southeast-1** (Singapore).
- Quyền IAM cho user tạo tài khoản: cần `AWSIoTFleetProvisioning` hoặc đơn giản là
  `AWSIoTFullAccess` (chỉ dùng lúc setup, thu hồi sau).
- Máy tính đã cài **ESP-IDF terminal** (để chạy `idf.py` và `tools/gen_certs_nvs.py`).
- AWS CLI (tùy chọn, chỉ cần nếu muốn làm bằng lệnh thay vì Console). Cài:
  `pip install awscli && aws configure` (nhập Access Key + Secret + region).

---

## 3. Tạo Thing + Certificate + Policy

### Cách A — AWS Console (dễ nhất cho người mới)

1. Vào **AWS Console → IoT Core** (Services → Internet of Things → IoT Core).
2. **Manage → All devices → Things → Create things**.
3. Chọn **Create a single thing** → đặt tên `gw_01` (phải khớp với `gateway_id` trong firmware).
   - Device shadow: để mặc định (không cần).
4. Ở bước **Device certificate**, chọn **Auto-generate a new certificate (recommended)**
   → bấm **Next**.
5. Ở bước **Policy**: bấm **Create policy** và điền:
   - **Name:** `irrigation_gw_policy`
   - **Policy statements** (thêm từng dòng, effect = Allow):

   | Action | Resource ARN |
   |---|---|
   | `iot:Connect` | `arn:aws:iot:<region>:<account-id>:client/*` |
   | `iot:Publish` | `arn:aws:iot:<region>:<account-id>:topic/irrigation/*` |
   | `iot:Receive` | `arn:aws:iot:<region>:<account-id>:topic/irrigation/*` |
   | `iot:Subscribe` | `arn:aws:iot:<region>:<account-id>:topicfilter/irrigation/*` |

   > `<account-id>` lấy ở My Security Credentials (góc phải). `<region>` = `ap-southeast-1`.
   > Nếu muốn mở rộng sau này, dùng `topic/*` và `topicfilter/*` thay vì chỉ `irrigation/*`.
   - Bấm **Create**.
6. Quay lại màn hình chọn Policy → tích chọn `irrigation_gw_policy` → **Create thing**.
7. **Tải 3 file** (quan trọng, màn hình hiện ra 1 lần duy nhất):
   - `xxxxxxxx-certificate.pem.crt` → đổi tên **`device_cert.pem`**
   - `xxxxxxxx-private.pem.key` → đổi tên **`private_key.pem`**
   - `AmazonRootCA1.pem` (link "Download Amazon Root CA 1")
   - (public key không cần dùng).
   - ⚠️ **Private key chỉ hiện 1 lần** — mất là phải tạo cert mới.

### Cách B — AWS CLI

```bash
# 1. Tạo key + CSR, rồi đăng ký cert (cách ngắn gọn hơn: create-keys-and-certificate)
aws iot create-keys-and-certificate \
  --set-as-active \
  --certificate-pem-outfile device_cert.pem \
  --private-key-outfile private_key.pem \
  --public-key-outfile public_key.pem

# Lấy certificate ARN từ output, ví dụ:
#   "certificateArn": "arn:aws:iot:ap-southeast-1:123456789012:cert/ABCD1234..."
CERT_ARN="arn:aws:iot:ap-southeast-1:123456789012:cert/ABCD1234..."

# 2. Tạo policy
aws iot create-policy \
  --policy-name irrigation_gw_policy \
  --policy-document '{
    "Version":"2012-10-17",
    "Statement":[
      {"Effect":"Allow","Action":"iot:Connect","Resource":"arn:aws:iot:ap-southeast-1:123456789012:client/*"},
      {"Effect":"Allow","Action":["iot:Publish","iot:Receive"],"Resource":"arn:aws:iot:ap-southeast-1:123456789012:topic/irrigation/*"},
      {"Effect":"Allow","Action":"iot:Subscribe","Resource":"arn:aws:iot:ap-southeast-1:123456789012:topicfilter/irrigation/*"}
    ]
  }'

# 3. Gắn policy vào cert
aws iot attach-policy --policy-name irrigation_gw_policy --target $CERT_ARN

# 4. Tạo Thing và gắn cert
aws iot create-thing --thing-name gw_01
aws iot attach-thing-principal --thing-name gw_01 --principal $CERT_ARN

# 5. Tải Amazon Root CA 1
curl -o AmazonRootCA1.pem https://www.amazontrust.com/repository/AmazonRootCA1.pem
```

---

## 4. Lấy Endpoint (địa chỉ broker)

- **Console:** IoT Core → **Settings** → ô **Device data endpoint**, copy dòng
  `xxxx-ats.iot.ap-southeast-1.amazonaws.com`.
- **CLI:**
  ```bash
  aws iot describe-endpoint --endpoint-type iot:Data-ATS
  # => "endpointAddress": "xxxx-ats.iot.ap-southeast-1.amazonaws.com"
  ```

Copy endpoint này vào `mqtt_broker_uri` của firmware (xem phần 6).

---

## 5. Flash certificate vào gateway

3 file PEM vừa tải để vào:

```
gateway_irr/tools/certs/
   ├─ AmazonRootCA1.pem
   ├─ device_cert.pem
   └─ private_key.pem
```

(Mở ESP-IDF terminal, `cd gateway_irr`, rồi:)

```bash
python tools/gen_certs_nvs.py --port COM10
```

Script sẽ:
1. Gói 3 PEM vào image NVS partition (`certs.bin`, 16KB).
2. Flashing partition `certs` (offset `0xD000`) — **không đụng firmware/app**.

> Đổi cert sau này (rotate): chỉ copy 3 file mới vào `tools/certs/` và chạy lại
> lệnh trên. Không cần reflash firmware.

---

## 6. Cấu hình firmware chuyển sang AWS

Có 2 cách:

### Cách 1 — sửa mặc định trong code (reflash firmware)
Trong `main/config.c` → `config_set_defaults()`:
```c
strlcpy(config->mqtt_broker_uri, "mqtts://xxxx-ats.iot.ap-southeast-1.amazonaws.com", ...);
config->mqtt_port = 8883;
config->mqtt_broker_type = MQTT_BROKER_AWS;   // <-- đổi từ HiveMQ sang AWS
```
Sau đó build + flash app bình thường.

### Cách 2 — ghi NVS lúc chạy (không reflash code)
Giữ code mặc định, nhưng lúc boot gọi (hoặc qua remote config) `config_save_broker_type(1)`
và lưu `mqtt_broker_uri` mới. Khởi động lại gateway.

Log期望 trên serial monitor:
```
MQTT_CLI: Using AWS IoT Core mutual-TLS (client_id=gw_01)
MQTT_CLI: MQTT connected to broker
MQTT_CLI: Subscribed to irrigation/HCM/gw_01/+/cmd
```

---

## 7. Kiểm tra end-to-end (MQTT test client)

1. IoT Core Console → **MQTT test client** → **Subscribe to a topic** → `irrigation/#`.
2. Bấm **Subscribe**. Sau ≤60s sẽ thấy gateway publish:
   - `irrigation/HCM/gw_01/status` → `{"status":"online",...}`
   - (khi có node báo cáo) `irrigation/HCM/gw_01/node_01/data`
3. **Publish** lên `irrigation/HCM/gw_01/node_01/cmd` payload:
   ```json
   {"cmd":"report"}
   ```
   → node phản hồi data (xác nhận 2 chiều).

---

## 8. (Tùy chọn) Lưu data vào AWS

Tạo **Rule** để đưa data đi tiếp: IoT Core → **Message routing → Rules → Create rule**.

SQL example (lấy mọi data topic):
```sql
SELECT *, topic(4) AS node_id FROM 'irrigation/+/+/node_+/data'
```
Action gợi ý:
- **Amazon Timestream** (time-series, tốt cho biểu đồ cảm biến).
- **AWS IoT SiteWise** (nếu làm lớn).
- **Lambda → DynamoDB** (lưu raw JSON).
- **S3** (archive).

> Lưu ý: mỗi action cần IAM Role cho phép IoT ghi vào dịch vụ đó.

---

## 9. Checklist lỗi thường gặp

| Triệu chứng | Nguyên nhân / cách fix |
|---|---|
| `AWS mode requires CA + client cert + key` | Chưa flash partition `certs` (phần 5). |
| Connect bị drop ngay | Client ID trùng (2 gateway cùng `gw_01` online) hoặc policy sai resource. |
| TLS handshake fail | Dùng sai Root CA (phải là `AmazonRootCA1.pem`), hoặc endpoint sai. |
| `iot:Connect` bị từ chối | Policy `Resource` client phải khớp `client/*` hoặc `client/gw_01`. |
| Subscribe thất bại | Policy thiếu `iot:Subscribe` trên `topicfilter/irrigation/*`. |
| Build lỗi TLS | Đảm bảo `CONFIG_MQTT_TRANSPORT_SSL=y` (đã set trong `sdkconfig.defaults`). |

---

## 10. Bảo mật

- **Private key tuyệt mát** — không commit vào git (đã ignore `tools/certs/` trong `.gitignore`).
- Thu hồi quyền `AWSIoTFullAccess` của IAM user sau khi setup xong.
- Định kỳ rotate certificate (tạo cert mới trên AWS, attach policy, rồi chạy lại
  `gen_certs_nvs.py` để flash — không cần reflash app).
- Nếu mất thiết bị: **detach + revoke** certificate trên AWS ngay.
