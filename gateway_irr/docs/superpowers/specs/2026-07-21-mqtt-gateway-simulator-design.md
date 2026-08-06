# MQTT Gateway Simulator - Design Document

**Date:** 2026-07-21
**Status:** Approved

## 1. Purpose

Standalone Windows executable (.exe) that simulates the ESP32 LoRa-to-MQTT gateway (from `gateway_irr` project) for testing the server-side MQTT pipeline without requiring physical hardware.

## 2. Scope

- **MQTT Data Simulator** — connects to a configurable MQTT broker, publishes sensor data on behalf of virtual nodes, subscribes to command topics, and simulates responses.
- LoRa UART, command cache, and retry logic are **not** simulated (they are irrelevant from the server's perspective).

## 3. Architecture

```
mqtt_gateway_sim.exe
├── main.py            — Entry point, Rich CLI (menu + live table + log panel)
├── mqtt_client.py     — paho-mqtt wrapper (connect, publish, subscribe, callback)
├── node_manager.py    — Virtual nodes (sensor data, online/offline, command response)
├── topic.py           — Topic builder (mirrors topic.h from firmware)
├── config.py          — CLI args + config file parser
├── requirements.txt
├── build.bat          — PyInstaller script
└── sim_config.json    — Optional configuration file
```

## 4. MQTT Topics

All topics follow the firmware convention:

| Topic pattern | Direction | Purpose |
|---|---|---|
| `irrigation/<site>/<gw>/node_<n>/data` | Sim → Server | Sensor data JSON |
| `irrigation/<site>/<gw>/node_<n>/status` | Sim → Server | Online/offline |
| `irrigation/<site>/<gw>/node_<n>/alarm` | Sim → Server | Alarm event |
| `irrigation/<site>/<gw>/status` | Sim → Server | Gateway heartbeat |
| `irrigation/<site>/<gw>/+/cmd` | Server → Sim | Subscribe to commands |

## 5. JSON Payloads

**Node data** (matches firmware `publish_node_data`):
```json
{"node":1,"soil":45,"temp":28,"hum":65,"pump":0,"battery":85,"timestamp":"2026-07-21T10:30:00Z"}
```

**Node status:**
```json
{"node":1,"status":"online"}
```

**Gateway status:**
```json
{"status":"online","nodes_registered":3,"commands_cached":0,"uptime_seconds":120,"version":"1.0.0","site":"factory_1","gateway_id":"gw_01"}
```

**Incoming commands** (from server):
```json
{"cmd":"on","duration":60}
{"cmd":"off"}
{"cmd":"set_threshold","low":30,"high":70}
{"cmd":"set_interval","value":300}
{"cmd":"report"}
{"cmd":"toggle"}
{"cmd":"set_mode","mode":1}
```

## 6. CLI Interface (Rich)

Three-panel layout:
- **Header** — identity (site, gw_id, broker status)
- **Node Table** — one row per node: ID, soil, temp, hum, battery, pump, online
- **Event Log** — timestamped messages (MQTT events, commands, status changes)
- **Footer Menu** — keyboard shortcuts

### Commands
| Key | Action |
|---|---|
| `1`-`9` | Toggle node online/offline |
| `S` | Start/Stop auto-publish |
| `P` | Manual publish for selected node |
| `A` | Publish alarm for selected node |
| `R` | Reconnect MQTT |
| `Q` | Quit |
| `?` | Help |

## 7. Interactive Startup

First run prompts for:
1. MQTT broker URI
2. Username / password (optional)
3. Site name (default: `factory_1`)
4. Gateway ID (default: `gw_01`)

These are saved to `sim_config.json` for subsequent runs.

## 8. Command Handling

When a command arrives from the server:
1. Log it in the event panel
2. If node is online: simulate ACK (show it in log), update pump state for `on`/`off`
3. If node is offline: log "node offline, command cached" (informational only — no actual cache logic)

## 9. Auto-Publish

- Configurable interval (default: 10s)
- Each online node publishes `data` at each tick
- Sensor data uses realistic random ranges: soil 20-80, temp 20-40°C, hum 40-90%, battery 50-100
- Gateway status publishes every 60s

## 10. Build

- Packaged with PyInstaller (`--onefile --console`)
- Dependencies: `paho-mqtt`, `rich`
- Output: single `mqtt_gateway_sim.exe` (~10MB)
