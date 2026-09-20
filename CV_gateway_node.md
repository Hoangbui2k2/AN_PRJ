# Smart Irrigation IoT System (ESP32 / ESP-IDF)

**Firmware Engineer (Gateway + Node)** · C · ESP-IDF · FreeRTOS · LoRa · MQTT/TLS

Firmware for a wireless irrigation system: an ESP32 **LoRa-to-MQTT gateway** bridging battery-powered ESP32 **sensor nodes** to a cloud MQTT broker (HiveMQ / AWS IoT Core), managing up to 10 nodes over unreliable radio and deep-sleep cycles.

### Gateway (ESP32 LoRa ↔ MQTT)
- Multi-task **FreeRTOS** with a producer/consumer UART queue so slow MQTT publishes never block LoRa RX.
- Custom **compact LoRa protocol** (UART2 @ 9600, CRC8): presence-bitmask uplink sends only changed sensor fields to save airtime.
- **Command cache-first delivery** for deep-sleeping nodes: FIFO cache flushed on node wake, with exponential-backoff retry and empty-ACK keep-alives.
- **Dual MQTT/TLS (8883)**: username/password and **AWS IoT Core mutual-TLS (X.509)** from an NVS cert partition.

### Node (ESP32 battery sensor)
- **Deep-sleep power optimization** (MOSFET sensor switch, EXT0/timer wake) minimizing LoRa TX.
- **Smart send-decision FSM**: boot baseline → delta-only compact updates → heartbeat, no re-send on unchanged data.
- Three irrigation modes (Manual / Schedule / Threshold) and **edge-triggered alarms** with NVS boot-recovery + gateway-lost fallback.

### Tech keywords
`C` · `ESP-IDF` · `FreeRTOS` · `ESP32` · `LoRa/SX1278` · `MQTT` · `TLS` · `AWS IoT Core` · `NVS` · `Deep-sleep` · `Protocol design` · `Git`
