# Skill: ESP-IDF Firmware for LoRa Irrigation Node

## Context

You are an expert ESP-IDF developer working on the **existing** firmware in `main/` for an ESP32 irrigation node. The node collects sensor data, controls a pump via CD4013 latching flip-flop, detects alarms with LED indication, and communicates with a Gateway over LoRa UART (E32-433T). You are **modifying / extending / debugging** the current code, not writing from scratch. Read the actual source before making changes — every constant below is checked against the real code.

---

## Code Style & Guidelines

- Match the existing style: `static` helpers, module header comment with `@brief`, `TAG` per module, `ESP_LOGx` everywhere.
- No dynamic allocation in the hot path; use stack buffers. Packets fit in `uint8_t buf[8]`.
- Persistent state lives in `RTC_DATA_ATTR` (`app_config_t`, command-dedup fingerprint).
- Thresholds & delta settings are mirrored to NVS (namespace `"node"`).
- All GPIO/`#define` constants are in `config.h` / module headers — **never hard-code pins in logic**.
- Respect the power budget: no long blocking delays in the normal path, prefer `vTaskDelay` short waits.

---

## Main Application Flow (main.c)

```text
app_main()
├── boot_led_flash()                  // LED x3 to confirm boot
├── nvs_flash_init() (erase+retry if needed)
├── wake_cause = power_get_wake_cause()
│
├── config_load()                     // RTC magic check → defaults if invalid
├── load_threshold_from_nvs()         // restore thresholds + delta from NVS
├── power_init()  sensors_init()  pump_init()  alarms_init()
├── lora_uart_init()  irrigation_init()
│
├── if (button_wake) {                // EXT0 (GPIO34)
│     LED flash; 50ms debounce;
│     if (button high again) → sleep;
│     pump_toggle()
│     power_sensor_on(); 7s stabilize; sensors_read(); power_sensor_off()
│     commands_send_data_with_ack()   // immediate status
│     commands_check_pending()        // short downlink listen
│     power_deep_sleep()
│   }
│
├── else { // timer wake or cold boot
│     power_sensor_on(); sensors_read()
│     check_and_update_alarms(&data)
│     commands_check_pending(&data)   // pre-irrigation listen
│     irrigation_cycle(&data)
│
│     // send decision (see Reporting below)
│     delta = node_check_delta_fields(&data)
│     exceeded = node_check_threshold(soil)
│     if   first-send     → commands_send_data_with_ack()
│     elif exceeded+delta → commands_send_compact_with_ack()
│     elif delta          → commands_send_compact_with_ack()
│     elif heartbeat due  → commands_send_heartbeat()
│     else                → skip (power save)
│
│     commands_check_pending(&data)   // post-uplink downlink window
│     if alarmCode active → send_alarm + alarms_signal + clear
│     pump_verify_state()
│     lora_sleep()                    // LoRa module into sleep
│     power_deep_sleep()
│   }
```

---

## Key Modules & Their Contracts

### config.h / config.c — the single source of truth

- `app_config_t` (packed, in RTC) holds interval, thresholds, schedule, mode, pump state, alarm, delta values, last-sent values. `config_get()` returns a pointer.
- `config_load()` validates `magic == "IRRG"`, otherwise `config_init_default()`.
- NVS API: `save_threshold_to_nvs()` / `load_threshold_from_nvs()` persist `thresholdLow/High` + the four `delta*`.
- Reporting API used by main: `node_check_threshold()`, `node_check_delta_fields()`, `node_store_last_sent*()`, `is_heartbeat_time()`, `increment/reset_cycle_counter()`.

### sensors.h / sensors.c

- `sensor_data_t` holds floats (`temperature`, `humidity`), raw/pct soil, `battery` (0xFF = external), and `sensor_error` bitmask (bit0=DHT, bit1=soil).
- DHT22 read: 2ms start, response + 40-bit timing, checksum, **3 attempts**.
- Soil: `map_soil_raw_to_pct()` on 16 averaged samples.
- Battery: GPIO36 divider (ratio 2), map 3.30–4.20V → 0–100%.

### pump.h / pump.c — CD4013 toggle

- `pump_toggle()` sends a 20ms HIGH pulse on GPIO23 (rising edge toggles Q1). Always sync `s_pump_state` and `cfg->pumpState` via `config_set_pump_state()`.
- Cold boot: `PUMP_COLDBOOT_STATE` (0 on this unit) is written to RTC so the tracked state matches the flip-flop.
- `pump_verify_state()` checks GPIO23 returns LOW at rest → else alarm 0x03.

### power.h / power.c — deep sleep

- Sensor power is **active-LOW** on GPIO26 (`power_sensor_on()` → LOW, `power_sensor_off()` → HIGH).
- `power_deep_sleep()`: enables timer wake (cfg->interval s) + EXT0 on GPIO34 (LOW), keeps `RTC_PERIPH` and `RTC_SLOW_MEM` ON (required for button wake + RTC config), holds GPIO26 via `rtc_gpio_hold_en`.

### irrigation.h / irrigation.c — 3 modes

- `irrigation_cycle(&data)`:
  1. If `pumpBySchedule` and duration elapsed → `pump_off()`, clear flag, restore `interval = normalInterval`.
  2. Schedule: `t` (slot) == target slot derived from `scheduleHour/Minute` (catch-up 1 slot), once per day via the slot-day counter → `pump_on()`, set `pumpBySchedule`, temporarily `interval = scheduleDuration`.
  3. Threshold: `should_irrigate` hysteresis (`s_irrigation_active`); soil sensor failure → alarm 0x01 + fallback to `MODE_SCHEDULE`.

### commands.h / commands.c — LoRa protocol

- Packet builders: `commands_build_data_packet` (0x01), `_compact_` (0x06), `_ack_` (0x03), `_alarm_` (0x04), `_heartbeat_` (0x02).
- Senders open a downlink window and return 0 if a valid downlink arrived, -1 on transport failure:
  - `commands_send_data_with_ack()`, `commands_send_compact_with_ack()`, `commands_send_heartbeat()`, `commands_send_alarm()`.
- Downlink processing: `commands_wait_downlink()` (after uplink) and `commands_check_pending()` (no uplink). Both validate CRC and node-id, execute via `commands_process()`, ACK 0x03, reset `gatewayLostCount`.
- `commands_process()` implements the 0x09 keep-alive no-op, command dedup (RTC fingerprint), and the switch over all 0x01–0x0A codes.

### lora_uart.h / lora_uart.c — E32-433T transport

- UART2 (GPIO16/17) @ 9600. MD0=GPIO22, MD1=GPIO21, AUX=GPIO19.
- `lora_send()`: force Normal mode, wait AUX ready, write bytes, wait full AUX LOW→HIGH TX-complete.
- `lora_sleep()`: MD0=MD1=1 (sleep). `lora_configure_module()` / `lora_read_configuration()` for E32 setup (config mode MD0=MD1=1).

### alarms.h / alarms.c — LED patterns

- Blink count == alarm code (1–5). Pattern repeated `ALARM_REPEAT_COUNT` (3) times.

### crc.h / crc.c

- `crc8_xor(buf, len)` = XOR of all bytes. `crc8_verify(pkt, 8)` checks byte7.

---

## Reporting / Delta Logic (main.c Step 9)

- A cycle **must** eventually reach `commands_send_data_with_ack`, `commands_send_compact_with_ack`, or `commands_send_heartbeat`, else nothing is sent.
- First-ever send uses `lastSentTemp == INT8_MIN` as the sentinel; afterwards `node_store_last_sent_compact()` updates only the sent fields.
- Soil-threshold trigger: only send if `delta != 0`, and force `presence |= PRESENCE_SOIL` — this prevents re-sending an unchanged soil value every wake.
- Heartbeat (`0x02`) resets `cyclesSinceSend`; skipping a cycle calls `increment_cycle_counter()` so the heartbeat eventually fires.

---

## Command Packet Processing (commands_process)

| cmd | Action |
| --- | --- |
| 0x01 interval | `p1|(p2<<8)`, min 5s, updates `interval`+`normalInterval` |
| 0x02 relay ON | `pump_on()`; duration>0 → `irrigation_start_timed_run(duration)` (RTC deadline); duration=0 → `irrigation_cancel_timed_run()` (manual hold) |
| 0x03 relay OFF | `pump_off()`, `irrigation_cancel_timed_run()` (clears deadline + restores interval) |
| 0x04 thresholds | `config_set_thresholds(p1,p2)` + NVS save (rejects low≥high) |
| 0x05 schedule | `config_set_schedule(p1,p2)` |
| 0x06 report | reset gw_lost, re-read sensors if needed, send 0x01 without downlink window |
| 0x07 toggle pump | `pump_toggle()` |
| 0x08 mode | validate ≤ MODE_THRESHOLD then `config_set_mode` |
| 0x09 keep-alive | handled **before** the switch: reset gw_lost, no ACK |
| 0x0A delta | clamp via `config_set_delta_*` + NVS save |
| default | ACK the valid downlink, log unknown |

Every executed command (except 0x09) ends with `commands_send_ack()`. Duplicate retry → ACK without re-execution (except 0x06).

---

## Downlink / ACK / Gateway-Lost Rules

- **Uplink → downlink window**: any valid 0x05 command, valid gateway 0x03 ACK, or 0x09 keep-alive resets `gatewayLostCount`.
- **No downlink after heartbeat** → `config_increment_gw_lost()`; at 3 → alarm 0x05 + `irrigation_gateway_lost()` (switch to Schedule, keep flags raised).
- **No ACK on**: CRC mismatch, wrong node_id, malformed frame (spec A).
- Never bump `gatewayLostCount` in the data/compact send path — only heartbeat (spec C).

---

## Build & Debug

```bash
idf.py build                       # or: idf.py -p COMx flash monitor
idf.py menuconfig                  # adjust thresholds, GPIO, deep-sleep timing
```

- Log level: `CONFIG_LOG_DEFAULT_LEVEL_INFO`. Tags: `MAIN`, `CONFIG`, `LORA_UART`, `SENSORS`, `PUMP`, `POWER`, `ALARMS`, `IRRIGATION`, `COMMANDS`.
- Power checks: monitor `Sleep Time`, `Cycles/Send`, `Failed ACKs`, `Pump Cycles` in the cycle summary log.

---

## Common Gotchas (double-check before touching)

- **Pump inversion**: `pump_toggle()` flips a CD4013 latch; always route state changes through `config_set_pump_state()` so RTC and `s_pump_state` agree. On cold boot both must equal `PUMP_COLDBOOT_STATE`.
- **Active-low sensor power**: `power_sensor_off()` = HIGH (never call `gpio_set_level` directly).
- **GPIO34 input-only**: no internal pull-up — external 10kΩ pull-up is required or the button wake never releases.
- **Deep sleep**: keep `RTC_PERIPH`/`RTC_SLOW_MEM` powered; hold GPIO26. Non-RTC GPIOs (MD0/21/22, AUX19, pump23) float → rely on PCB pull-downs.
- **CRC**: always XOR of all preceding bytes — both 8-byte (0–6) and 6-byte cmd (0–4) forms.
- **Command dedup fingerprint is in RTC** — it survives deep sleep, so a duplicate gateway retry across a sleep cycle is still ACKed without re-execution.

---

## Final Checklist

- [ ] GPIOs match `config.h` / module headers (LoRa 16/17/22/21/19, pump 23, sensor 26, DHT 33, soil 32, battery 36, button 34, LED 13).
- [ ] Deep sleep current < 100µA (measure).
- [ ] LoRa sends + ACK round-trip; compact/full/heartbeat selection works.
- [ ] Sensors read correctly; DHT retries; soil/battery error bits propagate.
- [ ] 3 modes (Manual/Schedule/Threshold) behave; schedule respects `lastWateringDay`.
- [ ] Gateway lost: heartbeat no-ACK → GW_LOST flag + Schedule fallback; recovery on valid downlink.
- [ ] Alarm detection (3 consecutive sensor/relay errors) + LED blink pattern + 0x04 packet.
- [ ] Config + delta thresholds persist across reset/deep sleep (RTC + NVS).
