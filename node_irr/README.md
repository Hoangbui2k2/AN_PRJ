# ESP32 LoRa Irrigation Node Firmware

Complete firmware for an ESP32-based irrigation node using ESP-IDF framework.

## Project Overview

This project implements a smart irrigation control system that:

- **Sensors**: DHT22 temperature/humidity, capacitive soil moisture, and battery voltage
- **Power**: MOSFET-controlled sensor power and deep sleep with timer + button wake sources
- **Control**: 3 operation modes (Manual, Schedule, Threshold) with fail-safe fallback
- **Communication**: LoRa UART (SX1278 / E32-433T) with delta-based reporting and heartbeat
- **Monitoring**: Alarm system with unique LED blink patterns and alarm packets
- **Configuration**: Persistent settings in RTC memory + NVS (thresholds, delta thresholds)

## Hardware Requirements

### Main Board Requirements

- ESP32-DevKitC (ESP32-WROOM-32)
- SX1278 UART Module (E32-433T or similar)
- DHT22 Temperature/Humidity Sensor
- Capacitive Soil Moisture Sensor
- CD4013BM96 Dual D Flip-Flop (latching pump toggle)
- AO3400A N-Channel MOSFET (pump driver) + MOSFET sensor power switch
- 5V Latching Relay (external supply)
- Tactile Button (external pull-up)
- LED with 1kΩ resistor
- 2×18650 Li-ion (parallel) + TP4056 charger + MT3608 boost (3.7-4.2V → 5V)

### GPIO Pin Mapping (FINAL — from code)

| Function | GPIO | Direction | Notes |
| --- | --- | --- | --- |
| LoRa TX (ESP32 → LoRa) | GPIO16 | Output | UART2 TX |
| LoRa RX (LoRa → ESP32) | GPIO17 | Input | UART2 RX |
| LoRa MD0 | GPIO22 | Output | Mode control |
| LoRa MD1 | GPIO21 | Output | Mode control |
| LoRa AUX | GPIO19 | Input | Status input |
| Pump Toggle (CD4013 CLOCK1) | GPIO23 | Output | 20ms HIGH pulse; LOW at rest |
| MOSFET Sensor Power | GPIO26 | Output | **active-LOW**: HIGH=OFF, LOW=ON |
| DHT22 DATA | GPIO33 | Bidirectional | 1-Wire, external 10kΩ pull-up |
| Soil Moisture ADC | GPIO32 | Input | ADC1_CH4 (12-bit) |
| Battery ADC | GPIO36 | Input | ADC1_CH0, 100k+100k divider |
| Manual Button | GPIO34 | Input | External pull-up, press LOW |
| Alarm LED | GPIO13 | Output | HIGH = ON |
| (Reserved) | GPIO0 | - | Boot mode, do not use |

### Critical Hardware Notes for Deep Sleep (<100µA)

To reach the <100µA deep-sleep target, external resistors are required for GPIOs that float during deep sleep:

1. **LoRa MD0 (GPIO22) / MD1 (GPIO21)** → add 10kΩ **pull-down** to GND. These GPIOs are NOT RTC-capable and float during deep sleep; without pull-downs the E32 module reverts to Normal mode and draws ~1-2 mA.
2. **LoRa AUX (GPIO19)** → add 10kΩ pull-down to GND (prevents floating input).
3. **Pump Toggle (GPIO23)** → add 10kΩ pull-down to GND (prevents false triggering).
4. **DHT22 DATA (GPIO33)** → external 10kΩ pull-**up** to 3.3 V (required by DHT22 protocol).
5. **Sensor Power MOSFET (GPIO26)** → RTC GPIO; the firmware holds its level during sleep with `rtc_gpio_hold_en`.
6. **Button (GPIO34)** → external 10kΩ pull-**up** to 3.3 V required (GPIO34 is input-only and has no internal pull-up).

## Technical Specifications

### Sensor Readings

- **DHT22**: temperature (°C, ×10) and humidity (%RH), 3 read attempts + checksum.
- **Soil moisture**: 16-sample averaged ADC → percentage. Dry = 4095 → 0%, wet = 2600 → 100%.
- **Battery**: GPIO36 divider (ratio 2) → 4.20V=100%, 3.30V=0%; read failure = 0xFF (external power).

### Operation Modes

1. **Manual (Mode 0)**: Button-controlled pump operation only (or remote command).
2. **Schedule (Mode 1)**: Time-based irrigation (once per day, needs time sync).
3. **Threshold (Mode 2)**: Soil moisture-based irrigation (default).

Gateway-lost fallback: no downlink after heartbeat → `gatewayLostCount++`; at 3 → GW_LOST flag + alarm + automatic switch to Schedule mode until a valid downlink restores.

### Alarm System

| Code | Alarm | Condition | LED blinks |
| --- | --- | --- | --- |
| 0x01 | Sensor Error | ≥3 consecutive DHT/soil failures | 1 |
| 0x02 | Soil Out of Range | soil <10% or >90% | 2 |
| 0x03 | Relay Error | pump toggle pulse failed / GPIO stuck | 3 |
| 0x04 | Low Battery | battery <20% (skip if 0xFF) | 4 |
| 0x05 | Gateway Lost | gatewayLostCount ≥ 3 | 5 |

Active alarm: 3× blink-pattern repeats, a 0x04 alarm packet sent, then alarm cleared.

### Power Management

- **Deep Sleep**: target <100µA; RTC_PERIPH + RTC_SLOW_MEM kept ON (button wake + RTC config).
- **MOSFET Control**: sensors powered on/off; LoRa module put to sleep between cycles.
- **Wake Sources**: GPIO button (EXT0, active-low) + timer (default 5 min).
- **Reporting economy**: no LoRa TX when nothing changed (delta-based reporting, heartbeat every 2 cycles).

## Software Architecture

### File Structure

```text
node_irr/
├── CMakeLists.txt              # project(irrigation_node)
├── sdkconfig.defaults          # build / debug / power config
├── main/
│   ├── CMakeLists.txt          # component SRCS
│   ├── main.c                  # entry point, boot flow, reporting decision
│   ├── config.h/c              # GPIO, constants, structs, RTC config, delta/NVS API
│   ├── lora_uart.h/c           # UART2, E32 modes, AUX/TX handling, config, sleep
│   ├── sensors.h/c             # DHT22 bit-bang, soil ADC, battery ADC
│   ├── pump.h/c                # CD4013 toggle, state, verify
│   ├── power.h/c               # sensor power MOSFET, button, deep sleep/wakeup
│   ├── alarms.h/c              # LED blink patterns
│   ├── irrigation.h/c          # 3-mode irrigation logic + gateway-lost fallback
│   ├── commands.h/c            # LoRa protocol, build/parse packets, ACK, dedup
│   └── crc.h/c                 # CRC8 (XOR-based)
```

### Key Features

#### 1. Configuration Management (`config.h/c`)

- Persistent RTC memory storage (`app_config_t`, magic `"IRRG"`).
- Thresholds + delta thresholds additionally persisted to **NVS** (namespace `"node"`).
- 3 operation modes, schedule timing, pump-state tracking.

#### 2. Sensor Integration (`sensors.h/c`)

- DHT22 with retry logic (up to 3 attempts) on GPIO33.
- Capacitive soil moisture ADC1_CH4 / GPIO32 (16 samples).
- Battery via ADC1_CH0 / GPIO36 (divider 2, 18650 curve).
- `sensor_error` bitmask (bit0=DHT, bit1=soil) propagates to packets.

#### 3. Power Management (`power.h/c`)

- Deep sleep with timer + EXT0 button wake (GPIO34).
- Sensor power MOSFET (active-low) + `rtc_gpio_hold_en` during sleep.
- Sleep duration configurable via LoRa (5s–3600s).

#### 4. LoRa Communication (`lora_uart.h/c`)

- UART2 (GPIO16/17) @ 9600; MD0(22)/MD1(21)/AUX(19).
- Automatic mode switching, AUX TX-complete wait, config-mode read/write, module sleep.

#### 5. Delta-Based Reporting (`main.c` + `config.h/c`)

- Sends full 0x01 baseline on first send, then **compact 0x06** packets with only changed fields.
- Sends **heartbeat 0x02** periodically (every `HEARTBEAT_CYCLES`).
- Sends nothing when all values are stable → LoRa TX stays off for power saving.

#### 6. Irrigation Control (`irrigation.h/c`)

- Three-mode operation + scheduled-duration pump control with interval restore.
- Threshold mode with hysteresis; sensor-error fallback to Schedule.

#### 7. Alarm System (`alarms.h/c`)

- Unique blink patterns per alarm (1–5 blinks), 3 repeats, LED helpers.

#### 8. Command Processing (`commands.h/c`)

- 6-byte downlink commands (0x01–0x0A) with CRC8, node-id match, no-ACK on bad CRC.
- ACK 0x03 after execution; command dedup (duplicate gateway retry ACKed without re-execution).
- 0x09 keep-alive resets gatewayLostCount without ACK.
- Downlink window (350ms) after every uplink; separate pending-window (250ms) listener.

### LoRa Packet Types

| Type | Value | Direction | Length | Purpose |
| --- | --- | --- | --- | --- |
| DATA | 0x01 | Node → GW | 8 | Full sensor baseline |
| HEARTBEAT | 0x02 | Node → GW | 8 | Periodic liveness packet |
| ACK | 0x03 | GW ↔ Node | 8 | Acknowledge / liveness |
| ALARM | 0x04 | Node → GW | 8 | Alarm report |
| CMD | 0x05 | GW → Node | 6 | Command packet |
| DATA_COMPACT | 0x06 | Node → GW | 4–8 | Compact (only changed fields) |

CRC8 = XOR of all preceding bytes (no 0x00 skip).

## Build Instructions

### Prerequisites

Install ESP-IDF (`export IDF_PATH`, or use the official installer). Requires `idf.py` on PATH.

### Build Steps

```bash
cd <project-directory>

# Build
idf.py build

# Flash + monitor
idf.py -p COMx flash monitor
```

### Configuration

```bash
idf.py menuconfig
```

Key settings (also in `sdkconfig.defaults`): console UART (UART0), flash size 4MB DIO 40MHz, compiler size optimization, task watchdog 10s, log level INFO.

## Usage

### Initial Boot

1. Power up the irrigation node.
2. LED flashes 3× to confirm boot; config loaded (defaults if RTC magic invalid).
3. Node enters deep sleep (default 5 min).

### Button Press (Manual Override)

- Single press toggles pump state (20ms CD4013 pulse).
- Status sent via LoRa, short downlink listen, then back to deep sleep.

### Deep Sleep Wake

1. **Timer wake**: full cycle — power sensors, read, alarm check, irrigation, delta/heartbeat reporting, downlink listen, alarm handling, sleep.
2. **Button wake**: pump toggle + immediate status report only.

### Gateway Commands (0x05 downlink)

- 0x01 set interval, 0x02/0x03 relay on/off, 0x04 thresholds, 0x05 schedule, 0x06 request report, 0x07 toggle pump, 0x08 set mode, 0x09 keep-alive, 0x0A set delta thresholds.

## Testing Checklist

### Static Checks

- [x] GPIO mapping matches code (`config.h` / module headers)
- [x] Deep sleep configuration optimized
- [x] CRC8 verification on all packet types
- [x] ACK + command dedup implemented
- [x] Alarm LED blink patterns per code
- [x] Pump state RTC persistence + CD4013 cold-boot sync

### Dynamic Tests

- [ ] Sensor initialization and error handling (DHT retries, soil/battery bits)
- [ ] LoRa UART communication (full/compact/heartbeat/alarm + ACK round-trip)
- [ ] Delta-based reporting and heartbeat selection
- [ ] Mode switching (Manual / Schedule / Threshold) + schedule duration
- [ ] Gateway-lost detection, Schedule fallback, and recovery
- [ ] Configuration persistence across reset/deep sleep (RTC + NVS)
- [ ] Deep sleep current < 100µA (measure)

## Production Notes

### Error Handling

- Sensor read retries (up to 3), LoRa send retries (up to 3), alarm activation on serious issues.
- Gateway-lost fallback to Schedule mode; alarm packet + LED pattern signalling.

### Performance

- Compact packets minimize airtime; no TX when nothing changed.
- Efficient deep-sleep management (LoRa module sleep, sensor power off, RTC GPIO hold).

### Debugging

- ESP_LOG with per-module TAGs (`MAIN`, `CONFIG`, `LORA_UART`, `SENSORS`, `PUMP`, `POWER`, `ALARMS`, `IRRIGATION`, `COMMANDS`).
- Cycle summary log shows mode, pump state, sensor values, threshold status, cycles/send, failed ACKs, alarm code, sleep time, pump cycles.

## License

This firmware is provided as-is for educational and development purposes. Use with caution in production environments.

## Support

For issues, see the complete code implementation in this repository. Refer to [AGENT.md](agent.md) for detailed specifications and [SKILL.md](skill.md) for implementation guidelines.

---

**Firmware Version**: 1.1.0
**Framework**: ESP-IDF
**Target**: ESP32-WROOM-32
**License**: Educational/Prototype
