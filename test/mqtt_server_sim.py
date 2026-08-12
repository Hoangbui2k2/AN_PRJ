#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
mqtt_server_sim.py — Mô phỏng SERVER cho hệ thống irrigation.

Đóng vai trò server MQTT để test hệ thống:
  Server (script này) <--MQTT--> Gateway <--LoRa--> Node

Chế độ hoạt động (mode):
  1. LISTEN (mặc định, -l):
       - Subscribe `irrigation/<site>/<gw>/#`
       - In từng message (topic + payload + timestamp)
       - Duy trì bảng trạng thái node / gateway
       - Cho phép gửi lệnh thủ công tới node

  2. SUITE (--suite file.json):
       - Đọc kịch bản test từ file JSON
       - Publish lệnh, chờ + verify payload nhận được (schema/giá trị)
       - In PASS/FAIL, ghi báo cáo `reports/<run_id>.json` + `.md`

  3. CHECK-SCHEMA (--check-schema <type> --timeout N):
       - Chờ 1 message thuộc loại data|status|alarm|gw_status
       - Validate theo schema của protocol mapping, in PASS/FAIL

Ví dụ:
  # Listen (server thủ công)
  python mqtt_server_sim.py --broker mqtts://host --port 8883 --user u --pass p \
      --site HCM --gw gw_01 --nodes 1,2

  # Chạy suite
  python mqtt_server_sim.py --broker mqtts://host --port 8883 --user u --pass p \
      --site HCM --gw gw_01 --suite suites/set_threshold.json

  # Kiểm tra schema 1 message
  python mqtt_server_sim.py --broker ... --check-schema data --timeout 30
"""

import argparse
import json
import os
import queue
import re
import ssl
import sys
import threading
import time
from datetime import datetime, timezone

try:
    import paho.mqtt.client as mqtt
except ImportError:
    print("Thiếu thư viện paho-mqtt. Cài bằng:  pip install paho-mqtt")
    sys.exit(1)

try:
    import certifi
    _CA_BUNDLE = certifi.where()
except ImportError:
    _CA_BUNDLE = None


# ──────────────────────────── Topic helpers ────────────────────────────

def node_topic(site, gw, node_id, suffix):
    """Topic node theo firmware topic.c: irrigation/<site>/<gw>/node_%02X/<suffix>"""
    return "irrigation/{}/{}/node_{:02X}/{}".format(site, gw, int(node_id), suffix)


def gw_topic(site, gw, suffix):
    return "irrigation/{}/{}/{}".format(site, gw, suffix)


def cmd_filter(site, gw):
    """Filter subscribe của gateway (MQTT_EVENT_CONNECTED)."""
    return "irrigation/{}/{}/+/cmd".format(site, gw)


# ──────────────────────────── Schema validation ────────────────────────────
# Theo MAPPING_GIAO_THUC_MQTT.md

SCHEMAS = {
    "data": {
        "required": ["node", "soil", "temp", "hum", "pump", "battery",
                     "timestamp", "threshold_exceeded"],
        "types": {
            "node": (int,), "soil": (int,), "temp": (int,), "hum": (int,),
            "pump": (int,), "battery": (int,), "threshold_exceeded": (bool,),
        },
        "ranges": {"soil": (0, 100), "hum": (0, 100), "pump": (0, 1),
                   "battery": (0, 255)},
        "extra_if_true": {"threshold_exceeded": ["threshold_low", "threshold_high"]},
    },
    "status": {
        "required": ["node", "status", "timestamp"],
        "types": {"node": (int,), "status": (str,), "timestamp": (int,)},
        "enums": {"status": ("online", "offline")},
    },
    "alarm": {
        "required": ["node", "type", "message"],
        "types": {"node": (int,), "type": (str,), "message": (str,)},
        # alarm do node gửi (3a) có: alarm_code, flags
        # alarm do gateway phát hiện ngưỡng (3b) có: soil, threshold_low, threshold_high
    },
    "gw_status": {
        "required": ["status"],
        "types": {"status": (str,)},
        "enums": {"status": ("online", "offline")},
        # offline (LWT) chỉ có status. online đầy đủ có thêm các field.
    },
}


def validate_payload(msg_type, payload):
    """Validate JSON payload theo schema. Trả về (ok: bool, errors: list[str])."""
    errors = []
    if msg_type not in SCHEMAS:
        return True, []
    schema = SCHEMAS[msg_type]

    if isinstance(payload, bytes):
        payload = payload.decode("utf-8", errors="replace")
    try:
        obj = json.loads(payload)
    except Exception as e:
        return False, ["payload không phải JSON: {}".format(e)]

    if not isinstance(obj, dict):
        return False, ["payload phải là object JSON"]

    for f in schema.get("required", []):
        if f not in obj:
            errors.append("thiếu field '{}'".format(f))

    for f, types in schema.get("types", {}).items():
        if f in obj and not isinstance(obj[f], types):
            errors.append("field '{}' sai kiểu (mong {})".format(
                f, "/".join(t.__name__ for t in types)))

    for f, (lo, hi) in schema.get("ranges", {}).items():
        if f in obj and isinstance(obj[f], (int, float)):
            if not (lo <= obj[f] <= hi):
                errors.append("field '{}' ngoài khoảng [{},{}] (={})".format(
                    f, lo, hi, obj[f]))

    for f, allowed in schema.get("enums", {}).items():
        if f in obj and obj[f] not in allowed:
            errors.append("field '{}' giá trị lạ '{}' (mong {})".format(
                f, obj[f], "/".join(allowed)))

    if msg_type == "data" and obj.get("threshold_exceeded") is True:
        for extra in schema["extra_if_true"]["threshold_exceeded"]:
            if extra not in obj:
                errors.append("threshold_exceeded=true nhưng thiếu '{}'".format(extra))

    return (not errors), errors


# ──────────────────────────── MQTT client wrapper ────────────────────────────

class ServerSim:
    def __init__(self, broker, port, user, password, site, gw,
                 nodes=None, insecure=False):
        self.broker = broker
        self.port = port
        self.user = user
        self.password = password
        self.site = site
        self.gw = gw
        self.nodes = nodes or [1, 2]
        self.insecure = insecure

        self.msgs = queue.Queue()          # (topic, payload_bytes)
        self.connected = threading.Event()
        self.connection_rc = None
        self._topic_ready = queue.Queue()  # ack subscribe để chắc chắn sẵn sàng

        try:
            self.client = mqtt.Client(
                mqtt.CallbackAPIVersion.VERSION2, protocol=mqtt.MQTTv311)
        except AttributeError:
            self.client = mqtt.Client(protocol=mqtt.MQTTv311)

        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message
        self.client.on_disconnect = self._on_disconnect
        self.client.on_subscribe = self._on_subscribe

        if self.user:
            self.client.username_pw_set(self.user, self.password)

        self._setup_tls()

    def _setup_tls(self):
        if self.broker.startswith("mqtts://") or self.port in (8883, 8884):
            if self.insecure:
                ca, req = None, ssl.CERT_NONE
            else:
                ca, req = _CA_BUNDLE, ssl.CERT_REQUIRED
            self.client.tls_set(ca_certs=ca, cert_reqs=req,
                                tls_version=ssl.PROTOCOL_TLS_CLIENT)

    def _on_connect(self, client, userdata, flags, reason_code, properties=None):
        # paho v2: reason_code là ReasonCode có .is_failure; v1: int
        try:
            ok = not reason_code.is_failure
            rc = str(reason_code)
        except AttributeError:
            ok = (reason_code == 0)
            rc = reason_code
        self.connection_rc = rc
        if ok:
            self.connected.set()
            print("[MQTT] Connected OK")
        else:
            print("[MQTT] Connect FAILED: rc={}".format(rc))

    def _on_disconnect(self, client, userdata, flags, reason_code, properties=None):
        try:
            rc = str(reason_code)
        except AttributeError:
            rc = reason_code
        print("[MQTT] Disconnected: {}".format(rc))

    def _on_message(self, client, userdata, message):
        self.msgs.put((message.topic, message.payload))

    def _on_subscribe(self, client, userdata, mid, reason_codes, properties=None):
        self._topic_ready.put(mid)

    # ── public API ──
    def connect(self, timeout=30):
        host = self.broker
        for scheme in ("mqtts://", "mqtt://", "tcp://", "ssl://"):
            if host.startswith(scheme):
                host = host[len(scheme):]
        print("[MQTT] Connecting to {}:{}".format(host, self.port))
        self.client.connect_async(host, self.port, keepalive=60)
        self.client.loop_start()
        if not self.connected.wait(timeout):
            print("[MQTT] Timeout connect")
            return False
        return True

    def subscribe(self, topic, qos=0):
        self.client.subscribe(topic, qos)

    def publish_cmd(self, node_id, payload_dict):
        topic = node_topic(self.site, self.gw, node_id, "cmd")
        data = payload_dict if isinstance(payload_dict, str) else json.dumps(payload_dict)
        info = self.client.publish(topic, data, qos=1)
        print("[SEND] {} <= {}".format(topic, data))
        return info

    def publish_raw(self, topic, payload):
        info = self.client.publish(topic, payload, qos=1)
        print("[SEND] {} <= {}".format(topic, payload))
        return info

    def drain(self, timeout=None):
        """Lấy 1 message trong queue (block tối đa timeout). Trả (topic, payload) hoặc None."""
        try:
            return self.msgs.get(timeout=timeout)
        except queue.Empty:
            return None


# ──────────────────────────── Suite engine ────────────────────────────

def topic_kind(topic, site, gw):
    """Phân loại topic nhận được. Trả về ('data'|'status'|'alarm'|'gw_status'|'other', node_id)."""
    m = re.search(r"node_([0-9A-Fa-f]{1,2})/(data|status|alarm|cmd)$", topic)
    if m:
        return m.group(2), int(m.group(1), 16)
    if topic.endswith("/status") and "/node_" not in topic:
        return "gw_status", None
    return "other", None


def check_message(sim, expect):
    """
    Kiểm tra 1 message nhận được có thoả kỳ vọng `expect` không.
    expect: dict gồm:
      - schema (optional): tên schema
      - topic_regex (optional): regex khớp topic
      - expect (optional): dict subset giá trị trong JSON payload
      - expect_raw (optional): so khớp raw payload string
    Trả (ok, error_str)
    """
    topic, payload = expect.get("_msg", (None, None))
    if topic is None:
        return False, "không có message"

    if isinstance(payload, bytes):
        payload = payload.decode("utf-8", errors="replace")

    # topic regex
    if "topic_regex" in expect:
        if not re.search(expect["topic_regex"], topic):
            return False, "topic '{}' không khớp regex '{}'".format(
                topic, expect["topic_regex"])

    # schema
    if expect.get("schema"):
        ok, errs = validate_payload(expect["schema"], payload)
        if not ok:
            return False, "schema fail: {}".format("; ".join(errs))

    # raw string
    if "expect_raw" in expect:
        if payload.strip() != str(expect["expect_raw"]).strip():
            return False, "raw khác: got='{}' want='{}'".format(
                payload, expect["expect_raw"])

    # JSON subset
    if "expect" in expect:
        try:
            obj = json.loads(payload)
        except Exception as e:
            return False, "payload không phải JSON: {}".format(e)
        for k, v in expect["expect"].items():
            if k not in obj:
                return False, "thiếu key '{}'".format(k)
            if obj[k] != v:
                return False, "key '{}' = {} (mong {})".format(k, obj[k], v)
    return True, ""


def run_suite(sim, suite_path, report_dir):
    with open(suite_path, "r", encoding="utf-8") as f:
        suite = json.load(f)

    name = suite.get("name", os.path.basename(suite_path))
    global_timeout = suite.get("timeout_seconds", 120)
    results = []
    start_all = time.time()

    print("\n===== SUITE: {} =====".format(name))
    print("Mô tả: {}".format(suite.get("description", "")))

    # subscribe tất cả topics một lần
    sim.subscribe("irrigation/{}/{}/#".format(sim.site, sim.gw), qos=1)
    time.sleep(1.0)  # chờ subscribe ack

    try:
        deadline = time.time() + global_timeout
        for step in suite.get("steps", []):
            action = step.get("action", "expect")
            step_name = step.get("name", action)
            print("\n-- Bước: {} [{}] --".format(step_name, action))

            if action == "publish_cmd":
                sim.publish_cmd(step["node"], step["cmd"])
                results.append((step_name, "PUBLISHED", True))
                continue

            if action == "publish_raw":
                sim.publish_raw(step["topic"], step["payload"])
                results.append((step_name, "PUBLISHED", True))
                continue

            if action == "sleep":
                secs = step.get("seconds", 1)
                print("  ngủ {}s".format(secs))
                time.sleep(min(secs, max(0, deadline - time.time())))
                results.append((step_name, "SLEPT", True))
                continue

            if action == "expect":
                step_timeout = step.get("timeout", 15)
                step_deadline = time.time() + min(step_timeout,
                                                  max(0, deadline - time.time()))
                matched = False
                while time.time() < step_deadline:
                    msg = sim.drain(timeout=min(1.0, max(0, step_deadline - time.time())))
                    if msg is None:
                        continue
                    expect = dict(step.get("expect_msg", {}))
                    expect["_msg"] = msg
                    ok, err = check_message(sim, expect)
                    if ok:
                        matched = True
                        print("  ✓ nhận được message thoả: {} {}".format(
                            msg[0], _short(msg[1])))
                        break
                    else:
                        print("  - bỏ qua message ({}): {} {}".format(
                            err, msg[0], _short(msg[1])))
                if matched:
                    results.append((step_name, "PASS", True))
                else:
                    results.append((step_name, "FAIL", False))
                continue

            print("  !! action không biết: '{}'".format(action))
            results.append((step_name, "UNKNOWN", False))

        elapsed = time.time() - start_all
        passed = sum(1 for r in results if r[2])
        print("\n===== KẾT QUẢ SUITE =====")
        print("  Pass: {}/{}".format(passed, len(results)))
        print("  Thời gian: {:.1f}s".format(elapsed))
        for name_, status, ok in results:
            print("    [{}] {}".format("PASS" if ok else "FAIL", name_))
    finally:
        sim.client.loop_stop()

    _write_report(report_dir, suite, results, elapsed=time.time() - start_all)
    return passed == len(results)


def _short(payload, n=120):
    if isinstance(payload, bytes):
        payload = payload.decode("utf-8", errors="replace")
    return payload if len(payload) <= n else payload[:n] + "..."


def _write_report(report_dir, suite, results, elapsed):
    os.makedirs(report_dir, exist_ok=True)
    run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    base = os.path.join(report_dir, "{}_{}".format(run_id, suite.get("name", "suite")))
    passed = sum(1 for r in results if r[2])
    report = {
        "suite": suite.get("name"),
        "run_id": run_id,
        "passed": passed,
        "total": len(results),
        "elapsed_seconds": round(elapsed, 1),
        "results": [{"step": n, "status": s, "ok": ok} for n, s, ok in results],
    }
    with open(base + ".json", "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)

    lines = ["# Báo cáo suite: {}".format(suite.get("name")),
             "",
             "- Run ID: `{}`".format(run_id),
             "- Kết quả: **{}/{} PASS**".format(passed, len(results)),
             "- Thời gian: {:.1f}s".format(elapsed),
             "",
             "| Bước | Trạng thái |",
             "|---|---|"]
    for n, s, ok in results:
        lines.append("| {} | {} |".format(n, "✅ PASS" if ok else "❌ FAIL"))
    with open(base + ".md", "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print("  Báo cáo: {}".format(base + ".md"))


# ──────────────────────────── Listen mode ────────────────────────────

class NodeView:
    def __init__(self):
        self.data = {}
        self.online = {}
        self.last = {}

    def update(self, kind, node_id, payload):
        try:
            obj = json.loads(payload) if isinstance(payload, str) else json.loads(payload.decode())
        except Exception:
            return
        if kind == "data":
            self.data[node_id] = obj
            self.online[node_id] = True
            self.last[node_id] = time.time()
        elif kind == "status":
            self.online[node_id] = (obj.get("status") == "online")
            if self.online[node_id]:
                self.last[node_id] = time.time()
        elif kind == "alarm":
            self.last[node_id] = time.time()
            self.online[node_id] = True

    def render(self):
        print("\n" + "=" * 72)
        print("  NODE TABLE")
        print("=" * 72)
        for nid in sorted(self.data.keys()):
            d = self.data[nid]
            st = "ONLINE" if self.online.get(nid) else "OFFLINE"
            print("  Node {:<3} | {} | soil={:<3} temp={:<3} hum={:<3} "
                  "pump={:<1} batt={:<3}".format(
                      nid, st, d.get("soil", "?"), d.get("temp", "?"),
                      d.get("hum", "?"), d.get("pump", "?"), d.get("battery", "?")))
        print("=" * 72)


def listen_mode(sim):
    sim.subscribe("irrigation/{}/{}/#".format(sim.site, sim.gw), qos=1)
    view = NodeView()
    print("\n[LISTEN] Bấm Ctrl+C để thoát. "
          "Gõ lệnh dạng: <node_id> <json>  (vd: 1 {\"cmd\":\"toggle\"})")

    def input_loop():
        while True:
            try:
                line = input("> ")
            except EOFError:
                return
            line = line.strip()
            if not line:
                continue
            parts = line.split(None, 1)
            if len(parts) < 2:
                print("  Định dạng: <node_id> <json>")
                continue
            try:
                node_id = int(parts[0])
                payload = json.loads(parts[1])
            except ValueError as e:
                print("  Lỗi parse: {}".format(e))
                continue
            sim.publish_cmd(node_id, payload)

    t = threading.Thread(target=input_loop, daemon=True)
    t.start()

    last_render = 0.0
    try:
        while True:
            msg = sim.drain(timeout=0.5)
            if msg is None:
                if time.time() - last_render > 15:
                    view.render()
                    last_render = time.time()
                continue
            topic, payload = msg
            now = datetime.now(timezone.utc).strftime("%H:%M:%S")
            txt = payload.decode("utf-8", errors="replace") if isinstance(payload, bytes) else payload
            print("[{}] {} => {}".format(now, topic, _short(txt, 160)))
            kind, node_id = topic_kind(topic, sim.site, sim.gw)
            if node_id is not None:
                view.update(kind, node_id, txt)
    except KeyboardInterrupt:
        print("\nThoát.")


# ──────────────────────────── Check-schema mode ────────────────────────────

def check_schema_mode(sim, msg_type, timeout):
    sim.subscribe("irrigation/{}/{}/#".format(sim.site, sim.gw), qos=1)
    print("[CHECK-SCHEMA] chờ 1 message loại '{}' (timeout {}s)...".format(msg_type, timeout))
    deadline = time.time() + timeout
    while time.time() < deadline:
        msg = sim.drain(timeout=min(1.0, max(0, deadline - time.time())))
        if msg is None:
            continue
        topic, payload = msg
        kind, node_id = topic_kind(topic, sim.site, sim.gw)
        if msg_type in ("gw_status",):
            is_kind = (kind == "gw_status")
        else:
            is_kind = (kind == msg_type and node_id is not None)
        if not is_kind:
            print("  - bỏ qua {} ({})".format(topic, kind))
            continue
        print("  + nhận: {} {}".format(topic, _short(payload, 200)))
        ok, errs = validate_payload(msg_type, payload)
        if ok:
            print("\n✅ SCHEMA PASS: message '{}' hợp lệ".format(msg_type))
        else:
            print("\n❌ SCHEMA FAIL:")
            for e in errs:
                print("    - {}".format(e))
        return ok
    print("\n❌ TIMEOUT: không nhận được message loại '{}'".format(msg_type))
    return False


# ──────────────────────────── main ────────────────────────────

def main():
    # Windows console mặc định cp1258 không in được tiếng Việt — ép stdout UTF-8
    for _stream in (sys.stdout, sys.stderr):
        try:
            _stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass

    ap = argparse.ArgumentParser(description="Mô phỏng server MQTT cho hệ thống irrigation")
    ap.add_argument("--broker", default="mqtts://d246c46a2ebe40d2ae0c787f92bfdbab.s1.eu.hivemq.cloud",
                    help="MQTT broker URI (mqtts://host hoặc host)")
    ap.add_argument("--port", type=int, default=8883)
    ap.add_argument("--user", default="", help="MQTT username")
    ap.add_argument("--pass", dest="password", default="", help="MQTT password")
    ap.add_argument("--site", default="HCM")
    ap.add_argument("--gw", default="gw_01")
    ap.add_argument("--nodes", default="1,2",
                    help="Danh sách node id (vd 1,2)")
    ap.add_argument("--insecure", action="store_true",
                    help="Bỏ qua verify TLS certificate")
    ap.add_argument("-l", "--listen", action="store_true",
                    help="Listen mode (in mọi message + gửi lệnh thủ công)")
    ap.add_argument("--suite", default="", help="Chạy suite JSON")
    ap.add_argument("--check-schema", choices=list(SCHEMAS.keys()),
                    help="Chờ 1 message và validate schema")
    ap.add_argument("--timeout", type=int, default=30,
                    help="Timeout cho check-schema / suite")
    ap.add_argument("--report-dir", default=os.path.join(os.path.dirname(__file__), "reports"))

    args = ap.parse_args()
    nodes = [int(x) for x in args.nodes.split(",") if x.strip()]

    sim = ServerSim(args.broker, args.port, args.user, args.password,
                    args.site, args.gw, nodes=nodes, insecure=args.insecure)

    if not sim.connect(timeout=30):
        sys.exit(1)

    if args.suite:
        ok = run_suite(sim, args.suite, args.report_dir)
        sys.exit(0 if ok else 1)
    elif args.check_schema:
        ok = check_schema_mode(sim, args.check_schema, args.timeout)
        sys.exit(0 if ok else 1)
    else:
        listen_mode(sim)


if __name__ == "__main__":
    main()
