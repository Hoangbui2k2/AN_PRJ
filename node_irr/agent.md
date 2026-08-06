# Agent Configuration for LoRa Irrigation Node (ESP-IDF)

## Project Overview

ESP32-based sensor node that:

- Collects environmental data (soil moisture, DHT22 temperature/humidity, battery)
- Controls pump via CD4013 latching flip-flop + AO3400A MOSFET
- Operates in 3 modes (Manual, Schedule, Threshold) with Schedule fallback on gateway loss
- Detects alarms (sensor error, soil out of range, relay error, low battery, gateway lost) with unique LED blink patterns
- Communicates with Gateway via LoRa UART (E32-433T / SX1278)
- **Delta-based reporting**: only sends compact packets when a field changes past a delta threshold; periodic heartbeat for liveness
- Uses a **downlink listen window** after each uplink where the gateway can flush commands / ACKs
- Optimized for low power (Deep Sleep + MOSFET sensor power control + LoRa module sleep)
- Persists config in RTC memory, and thresholds + delta settings in NVS

---

## Hardware Specifications

| Component | Model | Quantity |
| --- | --- | --- |
| MCU | ESP32 DevKit V1 (ESP-WROOM-32) | 1 |
| LoRa Module | SX1278 UART (E32-433T or equivalent) | 1 |
| Temp/Humidity | DHT22 (1-Wire bit-bang) | 1 |
| Soil Moisture | Capacitive (analog output) | 1 |
| Pump Driver | CD4013BM96 Dual D Flip-Flop (toggle latch) | 1 |
| Pump MOSFET | AO3400A (N-Channel, active-high pump drive) | 1 |
| Sensor Power MOSFET | MOSFET (active-LOW switch) | 1 |
| Latching Relay | 5V Magnetic Latching Relay | 1 |
| Button | Tactile Switch 6x6mm, external pull-up to 3.3V | 1 |
| Alarm LED | SMD LED + Resistor 1kΩ | 1 |
| Power | 2×18650 Li-ion (parallel) + TP4056 + Boost MT3608 | 1 |
| Battery divider | 100k+100k external divider → GPIO36 | 1 |

### Critical Hardware Notes for Deep Sleep (<100µA)

External resistors are required for GPIOs that float during deep sleep:

- LoRa MD0 (GPIO22) / MD1 (GPIO21) → 10kΩ **pull-down** to GND. Not RTC-capable; without
  them the E32 reverts to Normal mode and draws ~1-2 mA.
- LoRa AUX (GPIO19) → 10kΩ pull-down to GND (prevents floating input).
- Pump Toggle (GPIO23) → 10kΩ pull-down to GND (prevents false triggering).
- DHT22 DATA (GPIO33) → 10kΩ pull-**up** to 3.3 V (DHT22 protocol; internal pull-up may not suffice).
- Button (GPIO34) → 10kΩ pull-**up** to 3.3 V required (GPIO34 is input-only, no internal pull-up).
- SENSOR_POWER (GPIO26) is an RTC GPIO — the firmware holds its level during sleep with `rtc_gpio_hold_en`.

---

## GPIO Mapping (FINAL — from code)

| Function | GPIO | Direction | Notes |
| --- | --- | --- | --- |
| LoRa TX (ESP32 → LoRa) | GPIO16 | Output | UART2 TX |
| LoRa RX (LoRa → ESP32) | GPIO17 | Input | UART2 RX |
| LoRa MD0 | GPIO22 | Output | Mode control |
| LoRa MD1 | GPIO21 | Output | Mode control |
| LoRa AUX | GPIO19 | Input | Status input |
| Pump Toggle (CD4013 CLOCK1) | GPIO23 | Output | 20ms HIGH pulse; LOW at rest |
| MOSFET Sensor Power | GPIO26 | Output | **active-LOW**: HIGH=OFF, LOW=ON |
| DHT22 DATA | GPIO33 | Bidirectional | pull-up; bit-bang 1-Wire |
| Soil Moisture ADC | GPIO32 | Input | ADC1_CH4, ATTEN_DB_12 |
| Battery ADC | GPIO36 | Input | ADC1_CH0, 100k+100k divider (ratio 2) |
| Manual Button | GPIO34 | Input | External pull-up; press LOW |
| Alarm LED | GPIO13 | Output | HIGH=ON |
| (Reserved) | GPIO0 | - | Boot mode, do not use |

**Battery mapping** (18650): `pct = raw * 3.3V/4095 * 2` → map 4.20V=100%, 3.30V=0%. If the ADC read fails the battery is reported as `0xFF` (external power) and delta logic skips it.

**Soil mapping**: `pct = (4095 - raw) * 100 / (4095 - 2600)`. Dry = 4095 → 0%, wet = 2600 → 100%.

---

## LoRa Packet Types

| Type | Value | Direction | Length | Purpose |
| --- | --- | --- | --- | --- |
| DATA | 0x01 | Node → GW | 8 | Full sensor baseline |
| HEARTBEAT | 0x02 | Node → GW | 8 | Periodic liveness packet with current readings |
| ACK | 0x03 | GW ↔ Node | 8 | Acknowledge / gateway-liveness |
| ALARM | 0x04 | Node → GW | 8 | Alarm report |
| CMD | 0x05 | GW → Node | 6 | Command packet |
| DATA_COMPACT | 0x06 | Node → GW | 4–8 | Compact payload (only changed fields) |

### 8-byte Data Packet (`lora_data_packet_t`)

```text
byte0  node_id  (0x01-0xFE)
byte1  type
byte2  flags    bit0=pump bit1=threshold_exceeded bit2=gw_lost bit3=sensor_ok
byte3  soil moisture (0-100%)
byte4  temp      (offset +40; -40..+85 → 0..125)
byte5  humidity  (0-100%)
byte6  battery   (0-100% or 0xFF)
byte7  crc       CRC8 XOR of bytes 0-6
```

### 6-byte Command Packet (`lora_cmd_packet_t`)

```text
0  dest   0xFF=broadcast or Node ID
1  type   0x05
2  cmd    command code
3  param1
4  param2
5  crc    CRC8 XOR of bytes 0-4
```

### Compact Packet (0x06)

```text
0   node_id
1   type (0x06)
2   presence bitmask
3.. payload    (one byte per set bit, LSB-first)
last crc8      (XOR of all preceding bytes)
```

Presence bits:

- bit0 `PRESENCE_TEMP` → offset+40
- bit1 `PRESENCE_HUM` → 0-100
- bit2 `PRESENCE_SOIL` → 0-100
- bit3 `PRESENCE_BATTERY` → 0-100 / 0xFF
- bit4 `PRESENCE_FLAGS` → FLAG_* byte

Packet length = `3 + popcount(presence) + 1 (crc)`.

---

## Command Codes

| Code | Command | Param1 | Param2 |
| --- | --- | --- | --- |
| 0x01 | Set interval | low byte (s) | high byte (s) |
| 0x02 | Relay ON | duration lo (s, 0=inf) | duration hi |
| 0x03 | Relay OFF | - | - |
| 0x04 | Set thresholds | low (%) | high (%) |
| 0x05 | Set schedule | hour (0-23) | minute (0-59) |
| 0x06 | Request report | - | - |
| 0x07 | Toggle pump | - | - |
| 0x08 | Set mode | 0=Manual, 1=Schedule, 2=Threshold | - |
| 0x09 | **Sync time / keep-alive** | - | - |
| 0x0A | Set delta thresholds | type (0-3) | value (clamped) |

**0x09**: sent by the Gateway after an uplink when it has nothing else. It is a liveness no-op: the node resets `gatewayLostCount`, sends **no ACK**, and executes no command. It is handled before the command switch.

Delta threshold types for 0x0A:

- 0 = temperature (5–100 ×0.1 = 0.5–10.0°C)
- 1 = humidity (1–50%)
- 2 = soil (1–50%)
- 3 = battery (1–50%)

---

## Alarm Codes

| Code | Alarm | Condition | Blinks |
| --- | --- | --- | --- |
| 0x00 | None | - | - |
| 0x01 | Sensor error | ≥3 consecutive DHT/soil failures | 1 |
| 0x02 | Soil out of range | soil <10% or >90% (soil read OK) | 2 |
| 0x03 | Relay error | pump toggle pulse failed / GPIO stuck HIGH | 3 |
| 0x04 | Low battery | battery <20% (skip if 0xFF) | 4 |
| 0x05 | Gateway lost | gatewayLostCount ≥ 3 | 5 |

Active alarm is signalled with 3× blink-pattern repeats, a 0x04 alarm packet is sent once, then alarm is cleared. Blink count = alarm code.

---

## Operating Modes

### Mode 0: Manual

- Pump controlled only by the button or remote commands (0x02/0x07).
- `irrigation_should_irrigate()` returns the current `pumpState`, no automatic toggle.

### Mode 1: Schedule

- Requires a synchronized system clock (epoch ≥ 86400, i.e. one day).
- Ignores the day-of-year once per day (`lastWateringDay`), runs for `scheduleDuration`, then turns the pump off and restores `normalInterval`.
- While the scheduled pump is on, the deep-sleep interval is temporarily `scheduleDuration` so the node wakes to turn it off.

### Mode 2: Threshold (default)

- Pump ON when soil < `thresholdLow`; OFF when soil > `thresholdHigh` (hysteresis via `s_irrigation_active`).
- If the soil sensor fails, alarm 0x01 fires and the mode falls back to Schedule.

### Gateway Lost Fallback

- Heartbeat with no downlink response → `gatewayLostCount++`.
- At `gatewayLostCount ≥ GW_LOST_ALARM_THRESHOLD (3)` → `gatewayLost=true`, alarm 0x05, and `irrigation_gateway_lost()` switches mode to Schedule.
- `gatewayLostCount` stays raised (not reset) so FLAG_GATEWAY_LOST stays set; a later valid downlink calls `config_reset_gw_lost()`.

---

## Reporting Logic (main.c)

Priority per timer-wake cycle:

1. First send ever (`lastSentTemp == INT8_MIN`) → full 0x01 baseline.
2. Soil exceeds absolute thresholds → compact 0x06 with PRESENCE_SOIL + changed fields — only if something actually changed (never repeat the same soil value every cycle).
3. Any field delta → compact 0x06 (only changed fields).
4. Heartbeat due (`cyclesSinceSend >= HEARTBEAT_CYCLES=2`) → 0x02 heartbeat.
5. Otherwise → skip (LoRa TX stays off for power saving).

`node_check_delta_fields()` returns which presence bits changed; `node_store_last_sent_compact()` updates last-sent values per field so unchanged fields are not re-sent.

---

## LoRa Protocol / Gateway Timing

- UART2 @ 9600 baud. `lora_send()` ensures Normal mode, waits AUX ready, then waits a full AUX LOW→HIGH TX-complete cycle.
- `commands_*_with_ack()` = transmit, then open a **downlink window** `DOWNLINK_WINDOW_MS` (350 ms) via `commands_wait_downlink()`, retry up to `UPLINK_RETRIES` (3).
- `commands_wait_downlink()`: parse either a 6-byte CMD or 8-byte packet. Valid command → execute + send ACK 0x03 + reset `gatewayLostCount`. CRC mismatch or wrong node → no ACK. A legacy gateway ACK (0x03) is a liveness signal → reset count + clear alarm.
- `commands_check_pending()`: separate listen window `PENDING_WINDOW_MS` (250 ms) used when no uplink was just sent (button path + pre-irrigation read).
- **Command dedup**: last executed `(cmd,p1,p2)` stored in RTC; a duplicate gateway retry is ACKed **without re-execution** (keeps non-idempotent TOGGLE/PUMP safe). REPORT (0x06) is never deduplicated.

---

## Configuration Structure (RTC Memory)

```c
typedef struct __attribute__((packed)) {
    uint32_t magic;              /* "IRRG" */
    uint16_t interval;           /* deep-sleep (s), default 10 */
    uint8_t  thresholdLow;       /* soil-start %, default 30 */
    uint8_t  thresholdHigh;      /* soil-stop %, default 70 */
    uint8_t  scheduleHour;       /* schedule hour */
    uint8_t  scheduleMinute;     /* schedule minute */
    uint16_t scheduleDuration;   /* scheduled run (s), default 60 */
    uint8_t  mode;               /* operation_mode_t */
    uint8_t  nodeId;             /* 0x01-0xFE */
    uint8_t  sensorErrorCount;
    uint8_t  gatewayLostCount;
    bool     gatewayLost;
    bool     pumpState;
    bool     thresholdExceeded;
    uint16_t cyclesSinceSend;
    uint8_t  alarmCode;
    uint32_t totalPumpCycles;
    uint32_t lastScheduleTime;
    bool     pumpBySchedule;
    uint16_t normalInterval;     /* normal sleep interval to restore */
    uint16_t lastWateringDay;    /* day-of-year of last watering */
    int8_t   lastSentTemp;       /* °C×10, INT8_MIN = never sent */
    uint8_t  lastSentHumidity;
    uint8_t  lastSentSoil;
    uint8_t  lastSentBattery;
    uint8_t  lastSentFlags;
    uint8_t  deltaTemp;          /* °C×10 (default 20 = 2.0°C) */
    uint8_t  deltaHumidity;      /* % (default 5) */
    uint8_t  deltaSoil;          /* % (default 10) */
    uint8_t  deltaBattery;       /* % (default 10) */
} app_config_t;
```

Note: `config_load()` reads but never writes back (RTC-only). Thresholds and delta thresholds are additionally persisted to **NVS** which is restored on cold boot in `load_threshold_from_nvs()`.

---

## Code Structure

```text
node/
├── CMakeLists.txt              project(irrigation)
├── sdkconfig.defaults          build / debug / power config
├── main/
│   ├── CMakeLists.txt          SRCS list
│   ├── main.c                  entry point, boot flow, reporting decision
│   ├── config.h/c              GPIO, constants, structs, RTC config, delta/NVS API
│   ├── lora_uart.h/c           UART2, E32 modes, AUX/TX handling, serial config, sleep
│   ├── sensors.h/c             DHT22 bit-bang, soil ADC, battery ADC
│   ├── pump.h/c                CD4013 toggle, state, verify
│   ├── power.h/c               sensor power MOSFET, button, deep sleep/wakeup
│   ├── alarms.h/c              LED blink patterns
│   ├── irrigation.h/c          3-mode irrigation logic + gateway-lost fallback
│   ├── commands.h/c            LoRa protocol, build/parse packets, ACK, dedup
│   └── crc.h/c                 CRC8 (XOR-based)
```

## Tags & NVS

- **Log TAGs**: `"MAIN"`, `"CONFIG"`, `"LORA_UART"`, `"SENSORS"`, `"PUMP"`, `"POWER"`, `"ALARMS"`, `"IRRIGATION"`, `"COMMANDS"`.
- **NVS namespace**: `"node"` (keys `thr_low`, `thr_high`, `dt_temp`, `dt_hum`, `dt_soil`, `dt_bat`).

---

## Build & Test

```bash
idf.py build
idf.py -p COMx flash monitor   # adjust COM port; monitor shows wake + send/sleep logs
```

### Testing Checklist

- [ ] Boot: LED flashes 3×, serial shows config loaded and wake cause.
- [ ] Button wake: toggles pump, sends status, ACK handling.
- [ ] Timer wake: reads sensors, delta/heartbeat decision, command checks.
- [ ] LoRa TX/RX: verify full/compact/heartbeat/alarm and ACK round-trip.
- [ ] Deep sleep current < 100µA (measure with DMM).
- [ ] Command updates: interval, thresholds, mode, schedule, delta.
- [ ] Alarm detection: sensor error, low battery, soil out of range → LED + alarm packet.
- [ ] Gateway lost: heartbeat no-ACK → GW_LOST flag + Schedule fallback; restore on valid downlink.
- [ ] Threshold → NVS persistence across reset/deep sleep.
- [ ] Configuration (RTC) persistence across reset/deep sleep.

---

## Edge Cases / Gotchas

- **CD4013 reset state**: powers up with Q1 LOW on this unit; `PUMP_COLDBOOT_STATE=0` is synced to RTC on cold boot so `cfg->pumpState` and `pump_get_state()` never diverge (toggle inversion corruption).
- **Deep sleep power**: RTC_PERIPH + RTC_SLOW_MEM stay ON for EXT0 button wake and RTC config retention. `rtc_gpio_hold_en(SENSOR_POWER_GPIO)` holds sensor power off during sleep.
- **Non-RTC GPIOs float during sleep** → external pull-downs on the PCB (see Hardware Notes).
- **CRC8 for LoRa**: XOR of all preceding bytes (no 0x00 skip, no polynomial guess).
- The gateway-lost counter and the downlink-ACK logic are kept separate so that re-sends in the downlink window don't corrupt the counter.
