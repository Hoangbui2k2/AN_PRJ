# MQTT Gateway Simulator Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a standalone Windows `.exe` that simulates the ESP32 LoRa-to-MQTT gateway, enabling server-side testing without hardware.

**Architecture:** Rich console CLI app with paho-mqtt for MQTT, organized into focused modules: topic builder (mirrors firmware), virtual node manager, MQTT client wrapper, and main CLI entry point. Packaged via PyInstaller into a single .exe.

**Tech Stack:** Python 3.10+, paho-mqtt, rich, PyInstaller

**Output location:** `c:\project\gateway_irr\mqtt_gateway_sim\` (subdirectory of project, fully isolated from firmware code)

## Global Constraints

- All topics MUST match the firmware format: `irrigation/<site>/<gw>/node_<n>/<suffix>`
- Sensor data JSON MUST match firmware's `publish_node_data` structure
- Commands from server use `{"cmd": "<name>", ...}` format per firmware spec
- Dependencies only: paho-mqtt, rich (no web frameworks, no GUI libs)
- Package as single-file .exe with PyInstaller `--onefile --console`
- All Python files in `mqtt_gateway_sim/` directory, separate from firmware `main/`

---
### Task 1: Project Scaffold + Topic Builder

**Files:**
- Create: `mqtt_gateway_sim/__init__.py` (empty)
- Create: `mqtt_gateway_sim/topic.py`
- Create: `mqtt_gateway_sim/requirements.txt`

**Interfaces:**
- Produces: `topic_build(site, gateway_id, topic_type, node_id=0)` → str
- Produces: `topic_get_node_cmd_filter(site, gateway_id)` → str (subscribe filter with `+` wildcard)
- Produces: Constants `TOPIC_NODE_DATA`, `TOPIC_NODE_STATUS`, `TOPIC_NODE_ALARM`, `TOPIC_NODE_CMD`, `TOPIC_GW_STATUS`

- [ ] **Step 1: Create `__init__.py` and `requirements.txt`**

```python
# __init__.py - empty
```

```
# requirements.txt
paho-mqtt>=1.6.1
rich>=13.0.0
```

- [ ] **Step 2: Create `topic.py` with topic builder matching firmware**

```python
"""Topic builder — mirrors firmware topic.h exactly.

Topic format:
  irrigation/<site>/<gateway_id>/node_<node_id>/<suffix>  (node-level)
  irrigation/<site>/<gateway_id>/<suffix>                   (gateway-level)
"""

TOPIC_NODE_DATA = "data"       # /node_<id>/data
TOPIC_NODE_STATUS = "status"   # /node_<id>/status
TOPIC_NODE_ALARM = "alarm"     # /node_<id>/alarm
TOPIC_NODE_CMD = "cmd"         # /node_<id>/cmd
TOPIC_GW_STATUS = "status"     # /<gw>/status
TOPIC_GW_CONFIG = "config"     # /<gw>/config


def topic_build(site: str, gateway_id: str, topic_type: str, node_id: int = 0) -> str:
    """Build a full MQTT topic string matching firmware topic_build()."""
    if topic_type in (TOPIC_NODE_DATA, TOPIC_NODE_STATUS, TOPIC_NODE_ALARM, TOPIC_NODE_CMD):
        return f"irrigation/{site}/{gateway_id}/node_{node_id}/{topic_type}"
    else:
        return f"irrigation/{site}/{gateway_id}/{topic_type}"


def topic_get_node_cmd_filter(site: str, gateway_id: str) -> str:
    """Subscribe filter for all node command topics: uses '+' wildcard.

    Matches firmware topic_get_node_cmd_filter():
      irrigation/<site>/<gateway_id>/+/cmd
    """
    return f"irrigation/{site}/{gateway_id}/+/cmd"
```

- [ ] **Step 3: Verify topic.py works**

Run: `python -c "from mqtt_gateway_sim.topic import *; print(topic_build('factory_1','gw_01',TOPIC_NODE_DATA,1))"`
Expected: `irrigation/factory_1/gw_01/node_1/data`

Run: `python -c "from mqtt_gateway_sim.topic import *; print(topic_get_node_cmd_filter('factory_1','gw_01'))"`
Expected: `irrigation/factory_1/gw_01/+/cmd`

---

### Task 2: Virtual Node Manager

**Files:**
- Create: `mqtt_gateway_sim/node_manager.py`

**Interfaces:**
- Produces: `VirtualNode` class — id, online, soil, temp, hum, battery, pump_state
- Produces: `VirtualNode.generate_sensor_data()` → dict
- Produces: `VirtualNode.handle_command(cmd_dict)` → dict (response description)
- Produces: `NodeManager` class — add, remove, get, list, toggle, randomize all

- [ ] **Step 1: Create `node_manager.py`**

```python
"""Virtual node manager — simulates nodes without LoRa hardware."""

import random
import time
from dataclasses import dataclass, field
from typing import Optional


@dataclass
class VirtualNode:
    """A simulated irrigation node, matching node_entry_t in firmware."""
    id: int
    online: bool = True
    soil: int = 50        # 0-100 (%)
    temp: int = 28        # -30..80 (°C)
    hum: int = 65         # 0-100 (%)
    battery: int = 85     # 0-100 (%)
    pump_state: int = 0   # 0=off, 1=on
    last_seen: float = 0.0  # timestamp

    def __post_init__(self):
        self.last_seen = time.time()

    def generate_sensor_data(self) -> dict:
        """Generate realistic random sensor readings, matching firmware JSON."""
        self.soil = random.randint(20, 80)
        self.temp = random.randint(20, 40)
        self.hum = random.randint(40, 90)
        self.battery = max(5, self.battery + random.randint(-3, 0))
        self.last_seen = time.time()

        return {
            "node": self.id,
            "soil": self.soil,
            "temp": self.temp,
            "hum": self.hum,
            "pump": self.pump_state,
            "battery": self.battery,
        }

    def handle_command(self, cmd: str, params: dict) -> str:
        """Process a command from the server. Returns a human-readable response."""
        if cmd == "on":
            self.pump_state = 1
            duration = params.get("duration", 0)
            return f"Pump ON (duration={duration}s)"
        elif cmd == "off":
            self.pump_state = 0
            return "Pump OFF"
        elif cmd == "set_threshold":
            low = params.get("low", 30)
            high = params.get("high", 70)
            return f"Threshold set: low={low}, high={high}"
        elif cmd == "set_interval":
            value = params.get("value", 300)
            return f"Report interval set to {value}s"
        elif cmd == "report":
            return f"Immediate report requested — data follows"
        elif cmd == "toggle":
            self.pump_state = 1 if self.pump_state == 0 else 0
            return f"Pump toggled to {'ON' if self.pump_state else 'OFF'}"
        elif cmd == "set_mode":
            mode = params.get("mode", 1)
            return f"Mode set to {mode}"
        else:
            return f"Unknown command: {cmd}"

    def to_dict(self) -> dict:
        """Return sensor data dict with timestamp, matching firmware format."""
        data = self.generate_sensor_data()
        data["timestamp"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        return data

    def status_dict(self) -> dict:
        """Return status payload for /status topic."""
        return {
            "node": self.id,
            "status": "online" if self.online else "offline",
        }


class NodeManager:
    """Manages a collection of virtual nodes (max 10, matching firmware MAX_NODES)."""

    MAX_NODES = 10

    def __init__(self):
        self._nodes: dict[int, VirtualNode] = {}

    def add_node(self, node_id: int) -> Optional[VirtualNode]:
        """Add a node. Returns the node or None if at capacity."""
        if node_id in self._nodes:
            return self._nodes[node_id]
        if len(self._nodes) >= self.MAX_NODES:
            return None
        node = VirtualNode(id=node_id)
        self._nodes[node_id] = node
        return node

    def remove_node(self, node_id: int) -> bool:
        """Remove a node. Returns True if existed."""
        return self._nodes.pop(node_id, None) is not None

    def get_node(self, node_id: int) -> Optional[VirtualNode]:
        return self._nodes.get(node_id)

    def list_nodes(self) -> list[VirtualNode]:
        return list(self._nodes.values())

    def online_nodes(self) -> list[VirtualNode]:
        return [n for n in self._nodes.values() if n.online]

    def count(self) -> int:
        return len(self._nodes)

    def toggle_node(self, node_id: int) -> Optional[bool]:
        """Toggle a node's online status. Returns new state or None if not found."""
        node = self.get_node(node_id)
        if not node:
            return None
        node.online = not node.online
        return node.online

    def get_node_id_list(self) -> list[int]:
        return sorted(self._nodes.keys())
```

- [ ] **Step 2: Quick smoke test**

Run: `python -c "from mqtt_gateway_sim.node_manager import *; nm = NodeManager(); nm.add_node(1); n=nm.get_node(1); print(n.to_dict())"`
Expected: dict with node, soil, temp, hum, pump, battery, timestamp

---

### Task 3: MQTT Client Wrapper

**Files:**
- Create: `mqtt_gateway_sim/mqtt_client.py`

**Interfaces:**
- Consumes: `topic_build()`, `topic_get_node_cmd_filter()` from `topic.py`
- Consumes: `VirtualNode`, `NodeManager` from `node_manager.py`
- Produces: `MqttSimClient` class — connect, disconnect, publish_node_data, publish_node_status, publish_gw_status, on_command callback

- [ ] **Step 1: Create `mqtt_client.py`**

```python
"""MQTT client wrapper — handles connection, publishing, subscribing."""

import json
import logging
import time
from typing import Callable, Optional

import paho.mqtt.client as mqtt

from .topic import (
    topic_build,
    topic_get_node_cmd_filter,
    TOPIC_NODE_DATA,
    TOPIC_NODE_STATUS,
    TOPIC_NODE_ALARM,
    TOPIC_NODE_CMD,
    TOPIC_GW_STATUS,
)

logger = logging.getLogger("mqtt")


class MqttSimClient:
    """Wraps paho-mqtt to simulate gateway MQTT behavior."""

    def __init__(self, broker: str, port: int = 1883,
                 username: Optional[str] = None, password: Optional[str] = None,
                 site: str = "factory_1", gateway_id: str = "gw_01"):
        self.broker = broker
        self.port = port
        self.site = site
        self.gateway_id = gateway_id
        self.connected = False
        self.on_command: Optional[Callable[[int, str, dict], None]] = None

        self._client = mqtt.Client(
            client_id=f"gw_sim_{gateway_id}_{int(time.time())}",
            protocol=mqtt.MQTTv311,
        )
        if username and password:
            self._client.username_pw_set(username, password)

        self._client.on_connect = self._on_connect
        self._client.on_disconnect = self._on_disconnect
        self._client.on_message = self._on_message

    def _on_connect(self, _client, _userdata, _flags, rc):
        if rc == 0:
            self.connected = True
            logger.info(f"Connected to {self.broker}:{self.port}")

            # Subscribe to node command topics (matches firmware)
            cmd_filter = topic_get_node_cmd_filter(self.site, self.gateway_id)
            self._client.subscribe(cmd_filter, qos=1)
            logger.info(f"Subscribed to {cmd_filter}")

            # Subscribe to gateway config topic
            config_topic = topic_build(self.site, self.gateway_id, "config", 0)
            self._client.subscribe(config_topic, qos=1)
            logger.info(f"Subscribed to {config_topic}")

            # Publish gateway online status (matches firmware)
            self.publish_gw_status("online")
        else:
            logger.error(f"Connection failed, rc={rc}")

    def _on_disconnect(self, _client, _userdata, rc):
        self.connected = False
        if rc != 0:
            logger.warning(f"Unexpected disconnect (rc={rc})")
        else:
            logger.info("Disconnected")

    def _on_message(self, _client, _userdata, msg):
        """Handle incoming message — parse topic, extract node_id, call callback."""
        topic = msg.topic
        try:
            payload = json.loads(msg.payload.decode())
        except json.JSONDecodeError:
            logger.warning(f"Invalid JSON on {topic}: {msg.payload}")
            return

        logger.info(f"MQTT RX: {topic} => {payload}")

        # Parse node_id from topic: irrigation/<site>/<gw>/node_<n>/cmd
        if "/cmd" in topic and "/node_" in topic:
            try:
                # Extract node ID from /node_<n>/cmd
                parts = topic.split("/")
                for part in parts:
                    if part.startswith("node_"):
                        node_id = int(part.split("_")[1])
                        cmd = payload.get("cmd", "")
                        logger.info(f"Command for node {node_id}: {cmd}")
                        if self.on_command:
                            self.on_command(node_id, cmd, payload)
                        return
            except (IndexError, ValueError):
                logger.warning(f"Could not parse node_id from {topic}")

    def connect(self):
        """Connect to the MQTT broker (non-blocking)."""
        logger.info(f"Connecting to {self.broker}:{self.port}...")
        self._client.connect_async(self.broker, self.port, keepalive=60)
        self._client.loop_start()

    def disconnect(self):
        """Disconnect and publish offline status."""
        self.publish_gw_status("offline")
        self._client.disconnect()
        self._client.loop_stop()
        self.connected = False

    def publish(self, topic: str, payload: dict, qos: int = 1) -> bool:
        """Publish a JSON payload to a topic."""
        if not self.connected:
            logger.warning(f"Cannot publish: not connected")
            return False
        payload_str = json.dumps(payload, separators=(",", ":"))
        result = self._client.publish(topic, payload_str, qos=qos)
        if result.rc == mqtt.MQTT_ERR_SUCCESS:
            logger.debug(f"Published to {topic}: {payload_str}")
            return True
        logger.warning(f"Publish failed (rc={result.rc})")
        return False

    def publish_node_data(self, node) -> bool:
        """Publish sensor data for a node. Matches firmware publish_node_data()."""
        topic = topic_build(self.site, self.gateway_id, TOPIC_NODE_DATA, node.id)
        data = node.to_dict()
        return self.publish(topic, data)

    def publish_node_status(self, node, online: bool) -> bool:
        """Publish node online/offline status. Matches firmware publish_node_status()."""
        topic = topic_build(self.site, self.gateway_id, TOPIC_NODE_STATUS, node.id)
        payload = {"node": node.id, "status": "online" if online else "offline"}
        return self.publish(topic, payload)

    def publish_node_alarm(self, node) -> bool:
        """Publish alarm for a node. Matches firmware publish_node_alarm()."""
        topic = topic_build(self.site, self.gateway_id, TOPIC_NODE_ALARM, node.id)
        payload = {
            "node": node.id,
            "soil": node.soil,
            "type": "alarm",
            "message": "Sensor alarm triggered",
        }
        return self.publish(topic, payload)

    def publish_gw_status(self, status: str = "online", uptime: int = 0,
                          nodes_registered: int = 0, commands_cached: int = 0) -> bool:
        """Publish gateway status. Matches firmware gateway_status_task()."""
        topic = topic_build(self.site, self.gateway_id, TOPIC_GW_STATUS, 0)
        payload = {
            "status": status,
            "nodes_registered": nodes_registered,
            "commands_cached": commands_cached,
            "uptime_seconds": uptime,
            "version": "1.0.0",
            "site": self.site,
            "gateway_id": self.gateway_id,
        }
        return self.publish(topic, payload, qos=1)
```

---

### Task 4: Configuration Module

**Files:**
- Create: `mqtt_gateway_sim/config.py`
- Create: `mqtt_gateway_sim/sim_config.json` (default)

**Interfaces:**
- Produces: `SimConfig` — load/save from JSON, with fields: broker, port, username, password, site, gateway_id, interval

- [ ] **Step 1: Create `config.py`**

```python
"""Configuration loader — CLI args + JSON config file."""

import json
import os
import argparse
from dataclasses import dataclass, asdict
from typing import Optional

CONFIG_FILE = os.path.join(os.path.dirname(__file__), "sim_config.json")


@dataclass
class SimConfig:
    broker: str = "localhost"
    port: int = 1883
    username: str = ""
    password: str = ""
    site: str = "factory_1"
    gateway_id: str = "gw_01"
    interval: int = 10  # seconds between auto-publishes

    @classmethod
    def load(cls, path: Optional[str] = None) -> "SimConfig":
        path = path or CONFIG_FILE
        cfg = cls()
        if os.path.exists(path):
            try:
                with open(path) as f:
                    data = json.load(f)
                for key, value in data.items():
                    if hasattr(cfg, key):
                        setattr(cfg, key, value)
            except (json.JSONDecodeError, OSError):
                pass
        return cfg

    def save(self, path: Optional[str] = None):
        path = path or CONFIG_FILE
        with open(path, "w") as f:
            json.dump(asdict(self), f, indent=2)

    @classmethod
    def from_cli(cls) -> "SimConfig":
        """Parse CLI args, fall back to config file, then interactive prompts."""
        parser = argparse.ArgumentParser(description="MQTT Gateway Simulator")
        parser.add_argument("--broker", help="MQTT broker host")
        parser.add_argument("--port", type=int, help="MQTT broker port")
        parser.add_argument("--username", help="MQTT username")
        parser.add_argument("--password", help="MQTT password")
        parser.add_argument("--site", help="Site name")
        parser.add_argument("--gateway-id", help="Gateway ID")
        parser.add_argument("--interval", type=int, help="Auto-publish interval (s)")
        parser.add_argument("--config", help="Config file path")
        parser.add_argument("--no-interactive", action="store_true",
                            help="Skip interactive prompts, use defaults")
        args = parser.parse_args()

        cfg_path = args.config or CONFIG_FILE
        cfg = cls.load(cfg_path)

        # CLI overrides
        if args.broker:
            cfg.broker = args.broker
        if args.port:
            cfg.port = args.port
        if args.username:
            cfg.username = args.username
        if args.password:
            cfg.password = args.password
        if args.site:
            cfg.site = args.site
        if args.gateway_id:
            cfg.gateway_id = args.gateway_id
        if args.interval:
            cfg.interval = args.interval

        return cfg
```

- [ ] **Step 2: Create default `sim_config.json`**

```json
{
    "broker": "localhost",
    "port": 1883,
    "username": "",
    "password": "",
    "site": "factory_1",
    "gateway_id": "gw_01",
    "interval": 10
}
```

---

### Task 5: Main CLI Application (Rich Console)

**Files:**
- Create: `mqtt_gateway_sim/__main__.py`
- Modify: `mqtt_gateway_sim/__init__.py` (add version)

**Interfaces:**
- Consumes: all modules above
- Produces: runnable entry point with `python -m mqtt_gateway_sim`

- [ ] **Step 1: Create `__main__.py` with rich CLI**

```python
"""MQTT Gateway Simulator — Rich Console CLI main entry point."""

import logging
import signal
import sys
import time
import threading
from pathlib import Path

from rich.console import Console
from rich.layout import Layout
from rich.live import Live
from rich.table import Table
from rich.panel import Panel
from rich.text import Text
from rich.logging import RichHandler
from rich import box
from rich.prompt import Prompt

from .config import SimConfig
from .mqtt_client import MqttSimClient
from .node_manager import NodeManager

VERSION = "1.0.0"
console = Console()

# ---------------------------------------------------------------------------
# Logging setup — Rich handler for the log panel
# ---------------------------------------------------------------------------
logging.basicConfig(
    level=logging.INFO,
    format="%(message)s",
    datefmt="[%H:%M:%S]",
    handlers=[RichHandler(console=console, show_path=False, show_time=True)],
)
logger = logging.getLogger("sim")


# ---------------------------------------------------------------------------
# Application State
# ---------------------------------------------------------------------------
class AppState:
    def __init__(self, config: SimConfig):
        self.config = config
        self.node_mgr = NodeManager()
        self.mqtt: MqttSimClient = None
        self.running = True
        self.auto_publish = True
        self.start_time = time.time()
        self.log_lines: list[str] = []
        self._lock = threading.Lock()

    def uptime(self) -> int:
        return int(time.time() - self.start_time)


state: AppState = None


# ---------------------------------------------------------------------------
# MQTT Command Callback
# ---------------------------------------------------------------------------
def on_mqtt_command(node_id: int, cmd: str, payload: dict):
    """Handle incoming command from the server."""
    with state._lock:
        node = state.node_mgr.get_node(node_id)
        if not node:
            node = state.node_mgr.add_node(node_id)
            state.mqtt.publish_node_status(node, True)
            logger.info(f"[CMD] Auto-created node {node_id} from server command")

        if node.online:
            response = node.handle_command(cmd, payload)
            logger.info(f"[CMD] Node {node_id}: {response}")
            # Publish updated data after command
            state.mqtt.publish_node_data(node)
        else:
            logger.info(f"[CMD] Node {node_id} OFFLINE — command cached (simulated)")


# ---------------------------------------------------------------------------
# MQTT Background Thread
# ---------------------------------------------------------------------------
def mqtt_loop(config: SimConfig):
    """Connect MQTT and handle reconnection."""
    global state
    mqtt_client = MqttSimClient(
        broker=config.broker,
        port=config.port,
        username=config.username or None,
        password=config.password or None,
        site=config.site,
        gateway_id=config.gateway_id,
    )
    mqtt_client.on_command = on_mqtt_command
    mqtt_client.connect()
    state.mqtt = mqtt_client

    while state.running:
        if not mqtt_client.connected:
            time.sleep(5)
            try:
                mqtt_client.connect()
            except Exception:
                pass
        else:
            time.sleep(1)

    mqtt_client.disconnect()


# ---------------------------------------------------------------------------
# Auto-Publish Thread
# ---------------------------------------------------------------------------
def auto_publish_loop():
    """Periodically publish sensor data for online nodes."""
    global state
    while state.running:
        if state.auto_publish and state.mqtt and state.mqtt.connected:
            with state._lock:
                nodes = state.node_mgr.online_nodes()
            for node in nodes:
                try:
                    state.mqtt.publish_node_data(node)
                except Exception:
                    pass
                time.sleep(0.1)  # small gap between publishes
        time.sleep(state.config.interval)


# ---------------------------------------------------------------------------
# Gateway Status Publish Thread (every 60s)
# ---------------------------------------------------------------------------
def gw_status_loop():
    """Publish gateway heartbeat every 60s (matches firmware)."""
    global state
    while state.running:
        time.sleep(60)
        if state.mqtt and state.mqtt.connected:
            with state._lock:
                count = state.node_mgr.count()
            state.mqtt.publish_gw_status(
                "online",
                uptime=state.uptime(),
                nodes_registered=count,
                commands_cached=0,
            )


# ---------------------------------------------------------------------------
# Interactive Prompts (first run)
# ---------------------------------------------------------------------------
def interactive_setup(config: SimConfig) -> SimConfig:
    """Prompt user for connection details on first run."""
    console.print(Panel.fit(
        "[bold cyan]MQTT Gateway Simulator v1.0[/]\n"
        "Mô phỏng gateway ESP32 kết nối MQTT — không cần phần cứng!",
        border_style="cyan",
    ))
    console.print()

    config.broker = Prompt.ask(
        "[bold]MQTT Broker[/]", default=config.broker
    )
    port_str = Prompt.ask("[bold]Port[/]", default=str(config.port))
    config.port = int(port_str)

    if Prompt.ask("[bold]Use authentication?[/]", choices=["y", "n"], default="n") == "y":
        config.username = Prompt.ask("  Username", default=config.username)
        config.password = Prompt.ask("  Password", password=True, default="")

    config.site = Prompt.ask("[bold]Site name[/]", default=config.site)
    config.gateway_id = Prompt.ask("[bold]Gateway ID[/]", default=config.gateway_id)

    interval_str = Prompt.ask(
        "[bold]Auto-publish interval (seconds)[/]", default=str(config.interval)
    )
    config.interval = int(interval_str)

    config.save()
    return config


# ---------------------------------------------------------------------------
# Interactive Menu (keyboard input thread)
# ---------------------------------------------------------------------------
def menu_input_thread():
    """Handle keyboard input for the interactive menu."""
    global state
    import sys
    import select

    while state.running:
        # On Windows we need a different approach since select doesn't work on stdin
        try:
            cmd = input().strip().lower()
        except (EOFError, KeyboardInterrupt):
            break

        if not state.running:
            break

        with state._lock:
            if cmd == "q":
                state.running = False
                logger.info("Shutting down...")
                break
            elif cmd == "s":
                state.auto_publish = not state.auto_publish
                logger.info(
                    f"[MENU] Auto-publish {'STARTED' if state.auto_publish else 'STOPPED'}"
                )
            elif cmd == "p":
                # Publish all online nodes immediately
                if state.mqtt and state.mqtt.connected:
                    for node in state.node_mgr.online_nodes():
                        state.mqtt.publish_node_data(node)
                    logger.info("[MENU] Manual publish triggered")
                else:
                    logger.warning("[MENU] MQTT not connected")
            elif cmd.isdigit():
                nid = int(cmd)
                if nid == 0:
                    continue
                existing = state.node_mgr.get_node(nid)
                if existing:
                    new_state = state.node_mgr.toggle_node(nid)
                    if state.mqtt and state.mqtt.connected:
                        state.mqtt.publish_node_status(existing, new_state)
                    logger.info(
                        f"[MENU] Node {nid} → {'ONLINE' if new_state else 'OFFLINE'}"
                    )
                else:
                    node = state.node_mgr.add_node(nid)
                    if node:
                        if state.mqtt and state.mqtt.connected:
                            state.mqtt.publish_node_status(node, True)
                        logger.info(f"[MENU] Node {nid} created and ONLINE")
                    else:
                        logger.warning(f"[MENU] Max nodes ({NodeManager.MAX_NODES}) reached")
            elif cmd == "a":
                nid = Prompt.ask("  Node ID to alarm")
                try:
                    nid = int(nid)
                    node = state.node_mgr.get_node(nid)
                    if node and state.mqtt and state.mqtt.connected:
                        state.mqtt.publish_node_alarm(node)
                        logger.info(f"[MENU] Alarm published for node {nid}")
                    else:
                        logger.warning(f"[MENU] Node {nid} not found or MQTT disconnected")
                except ValueError:
                    logger.warning("[MENU] Invalid node ID")
                    logger.warning("  Unknown command. Try: 1-9, S, P, A, R, Q, ?")
            elif cmd == "r":
                logger.info("[MENU] Reconnecting MQTT...")
                if state.mqtt:
                    state.mqtt.disconnect()
                    time.sleep(1)
                    state.mqtt.connect()
            elif cmd == "?":
                console.print(Panel.fit(
                    "[bold]Commands:[/]\n"
                    "  [cyan]1-9[/]  Toggle node online/offline (or create new node)\n"
                    "  [cyan]S[/]    Start/Stop auto-publish\n"
                    "  [cyan]P[/]    Publish all online nodes immediately\n"
                    "  [cyan]A[/]    Publish alarm for a specific node\n"
                    "  [cyan]R[/]    Reconnect MQTT\n"
                    "  [cyan]Q[/]    Quit\n"
                    "  [cyan]?[/]    This help",
                    border_style="yellow",
                    title="Help",
                ))
            elif cmd:
                logger.warning(f"  Unknown command: {cmd}. Type '?' for help.")


# ---------------------------------------------------------------------------
# Node Table Builder
# ---------------------------------------------------------------------------
def build_node_table(node_mgr: NodeManager) -> Table:
    """Build a rich Table showing current node states."""
    table = Table(
        title="Nodes",
        box=box.ROUNDED,
        header_style="bold cyan",
        show_lines=True,
    )
    table.add_column("ID", width=4)
    table.add_column("Online", width=8)
    table.add_column("Soil", width=5)
    table.add_column("Temp", width=5)
    table.add_column("Hum", width=4)
    table.add_column("Batt", width=5)
    table.add_column("Pump", width=5)

    nodes = node_mgr.list_nodes()
    if not nodes:
        table.add_row("—", "—", "—", "—", "—", "—", "—")
    else:
        for n in sorted(nodes, key=lambda x: x.id):
            online_str = "[green]YES[/]" if n.online else "[red]OFF[/]"
            pump_str = "[green]ON[/]" if n.pump_state else "[dim]off[/]"
            table.add_row(
                str(n.id),
                online_str,
                str(n.soil),
                f"{n.temp}°",
                str(n.hum),
                str(n.battery),
                pump_str,
            )

    return table


# ---------------------------------------------------------------------------
# Main Entry Point
# ---------------------------------------------------------------------------
def main():
    global state

    # Load config, prompt if interactive
    config = SimConfig.from_cli()
    if not config.broker or config.broker == "localhost":
        config = interactive_setup(config)

    # Initialize state
    state = AppState(config)

    # Pre-populate 3 default nodes
    for nid in [1, 2, 3]:
        state.node_mgr.add_node(nid)

    # Start background threads
    threads = [
        threading.Thread(target=mqtt_loop, args=(config,), daemon=True),
        threading.Thread(target=auto_publish_loop, daemon=True),
        threading.Thread(target=gw_status_loop, daemon=True),
    ]
    for t in threads:
        t.start()

    # Give MQTT a moment to connect
    time.sleep(1.5)

    # Menu thread
    menu_thread = threading.Thread(target=menu_input_thread, daemon=True)
    menu_thread.start()

    logger.info("Gateway simulator started. Type '?' for help.")

    # Live display loop
    try:
        with Live(refresh_per_second=2, screen=True) as live:
            while state.running:
                layout = Layout()
                layout.split_column(
                    Layout(name="header", size=3),
                    Layout(name="body"),
                    Layout(name="footer", size=4),
                )

                # Header
                conn_str = (
                    "[green]CONNECTED[/]"
                    if state.mqtt and state.mqtt.connected
                    else "[red]DISCONNECTED[/]"
                )
                header_text = Text.assemble(
                    (" MQTT Gateway Simulator ", "bold cyan"),
                    (f" v{VERSION} ", "cyan"),
                    "\n",
                    (f" Site: {state.config.site}  ", "white"),
                    (f"GW: {state.config.gateway_id}  ", "white"),
                    (f"Broker: {state.config.broker}:{state.config.port}  ", "white"),
                    conn_str,
                    (f"  Uptime: {state.uptime()}s", "dim white"),
                )
                layout["header"].update(Panel(header_text, style="cyan", box=box.SIMPLE))

                # Body — node table
                layout["body"].update(build_node_table(state.node_mgr))

                # Footer — menu
                auto_str = "[green]ON[/]" if state.auto_publish else "[red]OFF[/]"
                footer_text = Text.assemble(
                    ("[1-9] Toggle node  ", "white"),
                    ("[S]tart/Stop auto  ", "white"),
                    (f"Auto: {auto_str}  ", "white"),
                    ("[P]ublish now  ", "white"),
                    ("[A]larm  ", "white"),
                    ("[R]econnect  ", "white"),
                    ("[Q]uit  ", "white"),
                    ("[?] Help", "white"),
                )
                layout["footer"].update(Panel(footer_text, style="white", box=box.SIMPLE))

                live.update(layout)
                time.sleep(0.5)

    except KeyboardInterrupt:
        pass
    finally:
        state.running = False
        if state.mqtt:
            state.mqtt.disconnect()
        logger.info("Gateway simulator stopped.")


if __name__ == "__main__":
    main()
```

Note: The menu input thread uses `input()` which blocks. In a real Rich app, we'd use `rich.prompt` for inline input. For simplicity and to avoid external deps (like `pynput`), we use a separate thread with blocking `input()`. The live display and input are on separate threads.

Actually, there's a problem — `input()` and `Live` conflict. Let me redesign the input mechanism. The best approach for Rich is to use a non-blocking input approach. On Windows, we can use `msvcrt.kbhit()` or `keyboard` module. But to keep it simple without extra deps, we'll use a different approach — an input prompt that pauses the live display.

Let me revise the approach: use a simple console mode where we show the live table, and the user presses keys that are captured by a background thread using `msvcrt` on Windows.

Actually, for cross-platform simplicity and minimal deps, let me take a different approach. Instead of a live updating table with keyboard capture, I'll use a more practical design:
- The live table refreshes automatically
- User types commands at the bottom (Rich `Prompt` in a separate thread)
- Or better: use the `keyboard` module

Wait, the simplest approach for Windows is `msvcrt` which is part of the standard library. Let me use that for keyboard input.

Let me rewrite main.py more carefully:

```python
"""MQTT Gateway Simulator — Rich Console CLI main entry point."""

import logging
import sys
import time
import threading
from typing import Optional

from rich.console import Console
from rich.layout import Layout
from rich.live import Live
from rich.table import Table
from rich.panel import Panel
from rich.text import Text
from rich.logging import RichHandler
from rich import box
from rich.prompt import Prompt

from .config import SimConfig
from .mqtt_client import MqttSimClient
from .node_manager import NodeManager

VERSION = "1.0.0"

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------
console = Console()
logging.basicConfig(
    level=logging.INFO,
    format="%(message)s",
    datefmt="[%H:%M:%S]",
    handlers=[RichHandler(console=console, show_path=False, show_time=True)],
)
logger = logging.getLogger("sim")


# ---------------------------------------------------------------------------
# Application State
# ---------------------------------------------------------------------------
class AppState:
    def __init__(self, config: SimConfig):
        self.config = config
        self.node_mgr = NodeManager()
        self.mqtt: Optional[MqttSimClient] = None
        self.running = True
        self.auto_publish = True
        self.start_time = time.time()
        self.last_cmd_result: str = ""

    def uptime(self) -> int:
        return int(time.time() - self.start_time)


state: Optional[AppState] = None


# ---------------------------------------------------------------------------
# MQTT Command Callback
# ---------------------------------------------------------------------------
def on_mqtt_command(node_id: int, cmd: str, payload: dict):
    """Handle incoming command from the server."""
    global state
    if not state:
        return
    node = state.node_mgr.get_node(node_id)
    if not node:
        node = state.node_mgr.add_node(node_id)
        if state.mqtt:
            state.mqtt.publish_node_status(node, True)
        logger.info(f"[CMD] Auto-created node {node_id} from server command")
        return

    if node.online:
        result = node.handle_command(cmd, payload)
        state.last_cmd_result = f"Node {node_id}: {result}"
        logger.info(f"[CMD] {state.last_cmd_result}")
        if state.mqtt:
            state.mqtt.publish_node_data(node)
    else:
        logger.info(f"[CMD] Node {node_id} OFFLINE — command cached (simulated)")


# ---------------------------------------------------------------------------
# MQTT Thread
# ---------------------------------------------------------------------------
def mqtt_loop(config: SimConfig):
    global state
    mqtt_client = MqttSimClient(
        broker=config.broker,
        port=config.port,
        username=config.username or None,
        password=config.password or None,
        site=config.site,
        gateway_id=config.gateway_id,
    )
    mqtt_client.on_command = on_mqtt_command
    mqtt_client.connect()
    if state:
        state.mqtt = mqtt_client

    while state and state.running:
        if not mqtt_client.connected:
            time.sleep(5)
            try:
                mqtt_client.connect()
            except Exception as e:
                logger.debug(f"MQTT reconnect failed: {e}")
        else:
            time.sleep(1)

    mqtt_client.disconnect()


# ---------------------------------------------------------------------------
# Auto-Publish Thread
# ---------------------------------------------------------------------------
def auto_publish_loop():
    global state
    while state and state.running:
        if state.auto_publish and state.mqtt and state.mqtt.connected:
            nodes = state.node_mgr.online_nodes()
            for node in nodes:
                try:
                    state.mqtt.publish_node_data(node)
                except Exception:
                    pass
        time.sleep(state.config.interval)


# ---------------------------------------------------------------------------
# Gateway Status Thread
# ---------------------------------------------------------------------------
def gw_status_loop():
    global state
    while state and state.running:
        time.sleep(60)
        if state.mqtt and state.mqtt.connected:
            count = state.node_mgr.count()
            state.mqtt.publish_gw_status(
                "online",
                uptime=state.uptime(),
                nodes_registered=count,
            )


# ---------------------------------------------------------------------------
# Interactive Setup
# ---------------------------------------------------------------------------
def interactive_setup(config: SimConfig) -> SimConfig:
    console.print(Panel.fit(
        "[bold cyan]MQTT Gateway Simulator v1.0[/]\n"
        "Simulate ESP32 LoRa-to-MQTT gateway — no hardware required!",
        border_style="cyan",
    ))
    console.print()

    config.broker = Prompt.ask("[bold]MQTT Broker[/]", default=config.broker)
    port_str = Prompt.ask("[bold]Port[/]", default=str(config.port))
    config.port = int(port_str)

    if Prompt.ask("[bold]Use authentication?[/]", choices=["y", "n"], default="n") == "y":
        config.username = Prompt.ask("  Username", default=config.username)
        config.password = Prompt.ask("  Password", password=True, default="")

    config.site = Prompt.ask("[bold]Site name[/]", default=config.site)
    config.gateway_id = Prompt.ask("[bold]Gateway ID[/]", default=config.gateway_id)
    interval_str = Prompt.ask("[bold]Auto-publish interval (s)[/]", default=str(config.interval))
    config.interval = int(interval_str)

    config.save()
    console.print("[green]Config saved![/]")
    return config


# ---------------------------------------------------------------------------
# Node Table
# ---------------------------------------------------------------------------
def build_node_table() -> Table:
    global state
    table = Table(box=box.ROUNDED, header_style="bold cyan", show_lines=True)
    table.add_column("ID", width=4)
    table.add_column("Online", width=8)
    table.add_column("Soil", width=5)
    table.add_column("Temp", width=5)
    table.add_column("Hum", width=5)
    table.add_column("Batt", width=5)
    table.add_column("Pump", width=5)

    if not state:
        return table

    nodes = state.node_mgr.list_nodes()
    if not nodes:
        table.add_row("—", "—", "—", "—", "—", "—", "—", style="dim")
    else:
        for n in sorted(nodes, key=lambda x: x.id):
            online_str = "[green]YES[/]" if n.online else "[red]OFF[/]"
            pump_str = "[green]ON[/]" if n.pump_state else "[dim]off[/]"
            table.add_row(
                str(n.id), online_str, str(n.soil),
                f"{n.temp}°", str(n.hum), str(n.battery), pump_str,
            )

    return table


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    global state

    config = SimConfig.from_cli()
    config.save()  # ensure config exists

    # Interactive setup on first run if default values
    if config.broker == "localhost":
        config = interactive_setup(config)

    state = AppState(config)

    # Pre-populate 3 nodes
    for nid in [1, 2, 3]:
        state.node_mgr.add_node(nid)

    # Start background threads
    threading.Thread(target=mqtt_loop, args=(config,), daemon=True).start()
    threading.Thread(target=auto_publish_loop, daemon=True).start()
    threading.Thread(target=gw_status_loop, daemon=True).start()

    time.sleep(1.5)

    logger.info("Gateway simulator started — type commands below.")
    logger.info("Commands: [1-9] toggle node, S auto, P publish, A alarm, R reconnect, Q quit, ? help")

    # Main loop: live display + input at bottom
    try:
        with Live(refresh_per_second=2, screen=True) as live:
            while state.running:
                layout = Layout()
                layout.split_column(
                    Layout(name="header", size=4),
                    Layout(name="table"),
                    Layout(name="input", size=3),
                )

                # Header
                conn = "[green]CONNECTED[/]" if (state.mqtt and state.mqtt.connected) else "[red]DISCONNECTED[/]"
                auto = "[green]ON[/]" if state.auto_publish else "[red]OFF[/]"
                h = Text.assemble(
                    (" MQTT Gateway Simulator ", "bold cyan"), (f"v{VERSION}", "cyan"), "\n",
                    (f" {state.config.site}/{state.config.gateway_id}", "white"),
                    (f" @ {state.config.broker}:{state.config.port}", "white"),
                    (" ── ", "dim"), conn, (" ── ", "dim"),
                    (f"Uptime: {state.uptime()}s", "dim white"), "\n",
                    (f" Auto-publish: {auto}", "white"),
                    (f" │ Nodes: {state.node_mgr.count()}", "white"),
                    (f" │ Online: {len(state.node_mgr.online_nodes())}", "white"),
                )
                layout["header"].update(Panel(h, box=box.SIMPLE, border_style="cyan"))

                # Table
                layout["table"].update(build_node_table())

                # Input prompt
                prompt_text = "Enter command ([cyan]?[/] for help): "
                layout["input"].update(Panel(
                    Text(prompt_text),
                    box=box.SIMPLE,
                    style="white on #222222",
                ))

                live.update(layout)
                time.sleep(0.5)

    except KeyboardInterrupt:
        pass
    finally:
        if state:
            state.running = False
        if state and state.mqtt:
            state.mqtt.disconnect()


if __name__ == "__main__":
    main()
```

Hmm, but the issue is that `Live` blocks the main thread and `input()` also blocks. The Rich approach with keyboard input during Live is tricky. Let me use `msvcrt` on Windows for non-blocking key reads. But that only works on Windows.

Actually, the most practical approach for this simulator is:

1. Use a simpler design where the "live" display is just a status bar that updates, and the main interaction loop uses Rich `Prompt`.
2. Or use a completely different approach: clear the screen each time, show the table, prompt for command.

Actually, I think the most practical and robust approach is:
- Run the live display in a background thread
- Use `input()` on the main thread for commands
- The live display thread reads shared state

But Rich's `Live` isn't thread-safe for updating. 

Alternative: Don't use Rich's `Live` at all. Instead:
- Use a simple console print approach with `os.system('cls')` to clear
- Show a table, then prompt
- Or, use an even better approach with Rich: print the table, then use a Prompt, then refresh

Actually the cleanest approach that works with `input()` is:
- Start with interactive setup using Rich prompts
- Then enter a loop: clear console, print table, prompt for action, execute action

Let me go with this simpler but very functional approach. It'll be much more robust.

```python
def main():
    # ... setup ...
    
    # Main interaction loop
    console.clear()
    while state.running:
        # Print header
        console.print(header_panel)
        # Print node table
        console.print(node_table)
        # Print command result if any
        # Prompt
        cmd = Prompt.ask("Command", default="")
        process_command(cmd)
        console.clear()
```

This is cleaner and avoids all the input conflicts. Let me write it this way.

OK wait, I should try to make it work with Live + a background input thread using msvcrt on Windows. That gives a much nicer experience. Let me check if msvcrt is available on Windows.

Yes, `msvcrt` is part of the Windows standard library (msvcrt = Microsoft Visual C Runtime). It provides `kbhit()` and `getch()` for non-blocking keyboard input.

Let me design it properly:

```python
import msvcrt
import sys

def get_key_nonblocking() -> Optional[str]:
    """Get a single keypress if available (Windows only)."""
    if msvcrt.kbhit():
        b = msvcrt.getch()
        # Arrow keys and function keys send two bytes
        if b == b'\xe0':
            b = msvcrt.getch()
            return None
        return b.decode('ascii', errors='ignore').lower()
    return None
```

This way the main thread can:
1. Run the Live display
2. Check for keypresses between refreshes
3. Process commands immediately

This gives a smooth experience. Let me use this approach.

Also I realize the log messages from RichHandler will show up in the Live display which could be messy. Let me remove RichHandler and just use a simple approach where I append log messages to a deque that the display shows.

Let me revise the whole approach to be cleaner.

Actually, thinking more about it, let me simplify significantly. The Rich Live display with separate log panel is the premium experience but has many threading pitfalls. Let me instead:

1. Use `msvcrt` for non-blocking keyboard input (Windows-only, which is fine since this targets Windows .exe)
2. The main thread runs the Live display and checks for keys between refresh
3. Log messages use a simple deque-based ring buffer
4. Background threads append to the ring buffer

This avoids RichHandler interfering with Live, and avoids input() blocking.

Let me write the final clean version.

Here's my plan for main.py:

```python
"""MQTT Gateway Simulator — Rich Console CLI main entry point."""

import logging
import time
import threading
import sys
from collections import deque
from typing import Optional

from rich.console import Console
from rich.layout import Layout
from rich.live import Live
from rich.table import Table
from rich.panel import Panel
from rich.text import Text
from rich import box
from rich.prompt import Prompt

from .config import SimConfig
from .mqtt_client import MqttSimClient
from .node_manager import NodeManager

VERSION = "1.0.0"
MAX_LOG = 50
console = Console()
logger = logging.getLogger("sim")


class AppState:
    def __init__(self, config: SimConfig):
        self.config = config
        self.node_mgr = NodeManager()
        self.mqtt: Optional[MqttSimClient] = None
        self.running = True
        self.auto_publish = True
        self.start_time = time.time()
        self.log: deque = deque(maxlen=MAX_LOG)
        self.last_cmd_result = ""

    def uptime(self) -> int:
        return int(time.time() - self.start_time)

    def log_info(self, msg: str):
        t = time.strftime("%H:%M:%S")
        self.log.append(f"[{t}] {msg}")


state: Optional[AppState] = None


def on_mqtt_command(node_id: int, cmd: str, payload: dict):
    if not state:
        return
    node = state.node_mgr.get_node(node_id)
    if not node:
        node = state.node_mgr.add_node(node_id)
        if state.mqtt:
            state.mqtt.publish_node_status(node, True)
        state.log_info(f"Auto-created node {node_id} from server")
        state.log_info(f"CMD from server: node={node_id}, cmd={cmd}")
        return

    if node.online:
        result = node.handle_command(cmd, payload)
        state.last_cmd_result = f"Node {node_id}: {result}"
        state.log_info(f"CMD → {state.last_cmd_result}")
        if state.mqtt:
            state.mqtt.publish_node_data(node)
    else:
        state.log_info(f"CMD → Node {node_id} OFFLINE (cached)")


def mqtt_loop(config: SimConfig):
    global state
    mqtt_client = MqttSimClient(
        broker=config.broker, port=config.port,
        username=config.username or None,
        password=config.password or None,
        site=config.site, gateway_id=config.gateway_id,
    )
    mqtt_client.on_command = on_mqtt_command
    mqtt_client.connect()
    if state:
        state.mqtt = mqtt_client
        state.log_info(f"Connecting to {config.broker}:{config.port}...")

    while state and state.running:
        if not mqtt_client.connected:
            time.sleep(5)
            try:
                mqtt_client.connect()
                state.log_info("Reconnecting MQTT...")
            except Exception:
                pass
        else:
            time.sleep(1)
    mqtt_client.disconnect()


def auto_publish_loop():
    global state
    while state and state.running:
        if state.auto_publish and state.mqtt and state.mqtt.connected:
            nodes = state.node_mgr.online_nodes()
            for node in nodes:
                try:
                    state.mqtt.publish_node_data(node)
                except Exception:
                    pass
        time.sleep(state.config.interval)


def gw_status_loop():
    global state
    while state and state.running:
        time.sleep(60)
        if state.mqtt and state.mqtt.connected:
            state.mqtt.publish_gw_status("online", uptime=state.uptime(), nodes_registered=state.node_mgr.count())


def interactive_setup(config: SimConfig) -> SimConfig:
    console.print(Panel.fit(
        "[bold cyan]MQTT Gateway Simulator v1.0[/]\n"
        "Simulate ESP32 LoRa-to-MQTT gateway — no hardware required!",
        border_style="cyan",
    ))
    config.broker = Prompt.ask("[bold]MQTT Broker[/]", default=config.broker)
    config.port = int(Prompt.ask("[bold]Port[/]", default=str(config.port)))
    if Prompt.ask("[bold]Authentication?[/]", choices=["y","n"], default="n") == "y":
        config.username = Prompt.ask("  Username", default=config.username)
        config.password = Prompt.ask("  Password", password=True, default="")
    config.site = Prompt.ask("[bold]Site name[/]", default=config.site)
    config.gateway_id = Prompt.ask("[bold]Gateway ID[/]", default=config.gateway_id)
    config.interval = int(Prompt.ask("[bold]Auto-publish interval (s)[/]", default=str(config.interval)))
    config.save()
    return config


def build_node_table() -> Table:
    table = Table(box=box.ROUNDED, header_style="bold cyan", title="Virtual Nodes")
    table.add_column("ID", width=4)
    table.add_column("Online", width=8)
    table.add_column("Soil", width=5)
    table.add_column("Temp", width=5)
    table.add_column("Hum", width=5)
    table.add_column("Batt", width=5)
    table.add_column("Pump", width=5)
    if not state:
        return table
    nodes = state.node_mgr.list_nodes()
    if not nodes:
        table.add_row("—", "—", "—", "—", "—", "—", "—", style="dim")
    else:
        for n in sorted(nodes, key=lambda x: x.id):
            online = "[green]YES[/]" if n.online else "[red]OFF[/]"
            pump = "[green]ON[/]" if n.pump_state else "[dim]off[/]"
            table.add_row(str(n.id), online, str(n.soil), f"{n.temp}°", str(n.hum), str(n.battery), pump)
    return table


def build_log_panel() -> Panel:
    if not state:
        return Panel("")
    lines = list(state.log)[-15:]  # show last 15 lines
    text = "\n".join(lines) if lines else "[dim]No events[/]"
    return Panel(Text(text), title="Event Log", box=box.ROUNDED, border_style="blue")


# Non-blocking key input on Windows
if sys.platform == "win32":
    import msvcrt
    def get_key() -> Optional[str]:
        if msvcrt.kbhit():
            b = msvcrt.getch()
            if b == b'\xe0':  # arrow/function key prefix
                msvcrt.getch()  # consume second byte
                return None
            try:
                return b.decode('ascii', errors='ignore').lower()
            except Exception:
                return None
        return None
else:
    def get_key() -> Optional[str]:
        return None  # fallback


def process_command(cmd: str):
    global state
    if not state:
        return
    if cmd == "q":
        state.running = False
        state.log_info("Shutting down...")
    elif cmd == "s":
        state.auto_publish = not state.auto_publish
        state.log_info(f"Auto-publish {'STARTED' if state.auto_publish else 'STOPPED'}")
    elif cmd == "p":
        if state.mqtt and state.mqtt.connected:
            for node in state.node_mgr.online_nodes():
                state.mqtt.publish_node_data(node)
            state.log_info("Manual publish — all online nodes")
        else:
            state.log_info("Cannot publish: MQTT not connected")
    elif cmd.isdigit():
        nid = int(cmd)
        if nid == 0:
            return
        existing = state.node_mgr.get_node(nid)
        if existing:
            new_state = state.node_mgr.toggle_node(nid)
            if state.mqtt and state.mqtt.connected:
                state.mqtt.publish_node_status(existing, new_state)
            state.log_info(f"Node {nid} → {'ONLINE' if new_state else 'OFFLINE'}")
        else:
            node = state.node_mgr.add_node(nid)
            if node:
                if state.mqtt and state.mqtt.connected:
                    state.mqtt.publish_node_status(node, True)
                state.log_info(f"Node {nid} created and ONLINE")
            else:
                state.log_info(f"Cannot create node {nid}: max {NodeManager.MAX_NODES}")
    elif cmd == "a":
        # We can't prompt here during live display, skip for now
        # User can use a command like "a1" to alarm node 1
        pass
    elif cmd.startswith("a") and len(cmd) > 1:
        try:
            nid = int(cmd[1:])
            node = state.node_mgr.get_node(nid)
            if node and state.mqtt and state.mqtt.connected:
                state.mqtt.publish_node_alarm(node)
                state.log_info(f"Alarm published for node {nid}")
            else:
                state.log_info(f"Alarm failed: node {nid} or MQTT")
        except ValueError:
            pass
    elif cmd == "r":
        state.log_info("Reconnecting MQTT...")
        if state.mqtt:
            state.mqtt.disconnect()
            time.sleep(0.5)
            state.mqtt.connect()
    elif cmd == "?":
        console.print(Panel.fit(
            "[bold]Commands:[/]\n"
            "  [cyan]1-9[/]  Toggle node online/offline (create if not exist)\n"
            "  [cyan]S[/]    Start/Stop auto-publish\n"
            "  [cyan]P[/]    Publish all online nodes now\n"
            "  [cyan]A<n>[/]  Publish alarm (e.g. A1 for node 1)\n"
            "  [cyan]R[/]    Reconnect MQTT\n"
            "  [cyan]Q[/]    Quit\n"
            "  [cyan]?[/]    This help",
            border_style="yellow", title="Help",
        ))
        input("Press Enter to continue...")


def main():
    global state

    config = SimConfig.from_cli()
    if config.broker == "localhost":
        config = interactive_setup(config)
    config.save()

    state = AppState(config)
    for nid in [1, 2, 3]:
        state.node_mgr.add_node(nid)

    threading.Thread(target=mqtt_loop, args=(config,), daemon=True).start()
    threading.Thread(target=auto_publish_loop, daemon=True).start()
    threading.Thread(target=gw_status_loop, daemon=True).start()

    time.sleep(1)
    state.log_info("Gateway simulator ready!")
    state.log_info("Type: 1-9=node, S=auto, P=publish, A<n>=alarm, R=reconnect, Q=quit")

    try:
        with Live(refresh_per_second=4, screen=True) as live:
            while state.running:
                layout = Layout()
                layout.split_column(
                    Layout(name="header", size=6),
                    Layout(name="main"),
                    Layout(name="footer", size=1),
                )

                # Header
                conn = "[green]CONNECTED[/]" if (state.mqtt and state.mqtt.connected) else "[red]DISCONNECTED[/]"
                auto = "[green]ON[/]" if state.auto_publish else "[red]OFF[/]"
                header = Panel(
                    Text.assemble(
                        (" MQTT Gateway Simulator ", "bold cyan"), (f"v{VERSION}", "cyan"), "\n",
                        (f" {config.site}/{config.gateway_id}", "white"),
                        (f" @ {config.broker}:{config.port}", "white"),
                        (" │ ", "dim"), conn, (" │ ", "dim"),
                        (f" Uptime: {state.uptime()}s", "dim white"), "\n",
                        (f" Auto: {auto}", "white"),
                        (f" │ Nodes: {state.node_mgr.count()}", "white"),
                        (f" │ Online: {len(state.node_mgr.online_nodes())}", "white"),
                    ),
                    box=box.SIMPLE, border_style="cyan",
                )
                layout["header"].update(header)

                # Main: table + log side by side
                main_layout = Layout()
                main_layout.split_row(
                    Layout(name="table", ratio=2),
                    Layout(name="log", ratio=3),
                )
                main_layout["table"].update(build_node_table())
                main_layout["log"].update(build_log_panel())
                layout["main"].update(main_layout)

                # Footer prompt
                layout["footer"].update(Panel(
                    "[bold cyan]Command[/] ([cyan]?[/] help) > ",
                    box=box.SIMPLE, style="white on #222222",
                ))

                live.update(layout)

                # Check for keypresses
                key = get_key()
                if key:
                    process_command(key)

                time.sleep(0.25)

    except KeyboardInterrupt:
        pass
    finally:
        if state:
            state.running = False
        if state and state.mqtt:
            state.mqtt.disconnect()


if __name__ == "__main__":
    main()
```

Wait, I realize the problem with `logger.info()` is that it goes to stderr via RichHandler which will mess up the Live display. Let me remove RichHandler and just use the state.log deque for all display. The background threads can call state.log_info() instead of logger.info().

But then MqttSimClient also uses logger... I need to either:
1. Make MqttSimClient use a callback or state.log_info
2. Or disable the RichHandler and use null handler, relying only on state.log

I think option 1 is cleaner. Let me pass a logging callback to MqttSimClient. Or better, have MqttSimClient accept a log_callback function that the main app sets.

Actually the simplest approach: in MqttSimClient and config.py, just use a plain logger with a NullHandler. Then the main app uses `state.log_info()` for everything, and the logging library just doesn't output anything (or goes to a file). The Live display shows the log.

But wait, the current code uses `logger.info` in several places. Let me separate concerns:
- Library code (mqtt_client.py, node_manager.py, topic.py, config.py) uses standard `logging` → goes to null handler or file
- Main app uses `state.log_info()` for UI display

Let me restructure:

1. Keep library logging simple (null handler)
2. In main app, replace all logger.info calls with state.log_info
3. MqttSimClient gets a log_callback for important events

Or, simpler: just disable the rich handler and don't use logging for display at all. Use state.log_info() everywhere. The library modules don't need to log to console.

Let me finalize the design. I'll write clean code now.

Actually, for simplicity in the build, let me ensure:
- `logging` library is configured to NOT output to console (so it doesn't interfere with Live)
- All display goes through `state.log_info()` 
- MqttSimClient has an optional `log_callback`

I think the best approach: since we're building a simulator not a production service, let me keep it simple. I'll adjust MqttSimClient to use state.log_info via a callback, not logger.

OK, let me write everything cleanly now. I have a clear design in mind.<｜end▁of▁thinking｜>Bây giờ mình sẽ implement toàn bộ dự án. Đây là kế hoạch chi tiết:

<｜｜DSML｜｜tool_calls>
<｜｜DSML｜｜invoke name="TodoWrite">
<｜｜DSML｜｜parameter name="todos" string="false">[{"content":"Task 1: Tạo project scaffold + topic builder","status":"in_progress","activeForm":"Creating project scaffold and topic builder"},{"content":"Task 2: Virtual Node Manager","status":"pending","activeForm":"Creating virtual node manager"},{"content":"Task 3: MQTT Client Wrapper","status":"pending","activeForm":"Creating MQTT client wrapper"},{"content":"Task 4: Configuration Module","status":"pending","activeForm":"Creating configuration module"},{"content":"Task 5: Main CLI App + build script","status":"pending","activeForm":"Creating main CLI application"},{"content":"Build .exe với PyInstaller","status":"pending","activeForm":"Building executable"}]