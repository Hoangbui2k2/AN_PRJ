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

  2. SUITE (--python mqtt_server_sim.py --broker mqtts://d246c46a2ebe40d2ae0c787f92bfdbab.s1.eu.hivemq.cloud --port 8883 --user hivemq.webclient.1742180699133 --pass "#x1V7:H62pCZ%e&nGkgR" --site HCM --gw gw_01 --nodes 1,2 --suite suites/baseline_provision.json file.json):
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
    "baseline": {
        # irrigation/<site>/<gw>/node_XX/baseline
        #   request : node xin baseline (node tự gửi REQ 0x09 khi thiếu)
        #   done    : node đã lưu xong 1 series (uplink 0x08)
        "required": ["node", "status"],
        "types": {"node": (int,), "status": (str,),
                  "series": (int,), "version": (int,), "series_mask": (int,)},
        "enums": {"status": ("request", "done")},
        "ranges": {"series": (0, 2), "series_mask": (0, 7), "version": (1, 255)},
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
    m = re.search(r"node_([0-9A-Fa-f]{1,2})/(data|status|alarm|cmd|baseline)$", topic)
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

    # Message KHÔNG khớp bước đang chờ được GIỮ LẠI ở đây thay vì vứt đi: khi có
    # nhiều node cùng hoạt động, thứ tự tới không xác định (request của node 2 có
    # thể vượt request của node 1), nên bước sau vẫn phải match được chúng.
    pending = []
    PENDING_MAX = 300

    def _msg_matches(msg, expect_msg):
        exp = dict(expect_msg or {})
        exp["_msg"] = msg
        return check_message(sim, exp)

    def _payload_of(msg):
        raw = msg[1]
        if isinstance(raw, bytes):
            raw = raw.decode("utf-8", errors="replace")
        try:
            return json.loads(raw)
        except Exception:
            return None

    # ── Tự động cấp lại bảng baseline khi gateway MẤT bảng ──
    # Session baseline của gateway chỉ nằm trong RAM: nếu gateway reset giữa lúc
    # đang gửi chunk (đã gặp: 'rst:0x1 (POWERON_RESET)' ngay sau 'TX complete'),
    # bảng mất và node sẽ REQ mãi mà không bao giờ 'done'. Gateway phát hiện và
    # publish 'needed=1'; runner gửi lại đúng message set_baseline đã push trước đó.
    last_cmd = {}       # node_id -> payload set_baseline gần nhất
    repush_count = {}   # node_id -> số lần đã gửi lại
    warned_no_cmd = set()   # node_id -> đã cảnh báo "chưa có bảng để gửi lại"
    repush_last = {}    # node_id -> thời điểm gửi lại gần nhất (giây)
    REPUSH_MAX = 5
    REPUSH_MIN_GAP_S = 5.0

    def _maybe_repush(msg):
        obj = _payload_of(msg)
        if not obj or obj.get("status") != "request" or not obj.get("needed"):
            return
        nid = obj.get("node")
        cmd = last_cmd.get(nid)
        if cmd is None:
            # Suite chủ động KHÔNG push bảng cho node này với (ví dụ
            # baseline_relay_sequential chỉ push khi nhận được request đã relay).
            if nid not in warned_no_cmd:
                warned_no_cmd.add(nid)
                print("  ! Gateway cần lại bảng baseline cho node {} nhưng suite chưa "
                      "từng publish - bỏ qua (cảnh báo chỉ in 1 lần)".format(nid))
            return
        if repush_count.get(nid, 0) >= REPUSH_MAX:
            if nid not in warned_no_cmd:
                warned_no_cmd.add(nid)
                print("  ! Node {} vẫn cần bảng sau {} lần gửi lại - bỏ qua".format(
                    nid, REPUSH_MAX))
            return
        now_s = time.time()
        if now_s - repush_last.get(nid, 0.0) < REPUSH_MIN_GAP_S:
            return
        repush_last[nid] = now_s
        repush_count[nid] = repush_count.get(nid, 0) + 1
        print("  ↻ Gateway MẤT bảng của node {} → tự động push lại ({}/{}): {}".format(
            nid, repush_count[nid], REPUSH_MAX, _short(json.dumps(cmd))))
        sim.publish_cmd(nid, cmd)

    step_name = "(chưa bắt đầu)"
    interrupted = False
    try:
        deadline = time.time() + global_timeout
        for step in suite.get("steps", []):
            action = step.get("action", "expect")
            step_name = step.get("name", action)
            print("\n-- Bước: {} [{}] --".format(step_name, action))

            # 'needed' có thể đã bị đệm từ bước trước → xử lý TRƯỚC khi bước này
            # tiêu thụ nó (nếu để bước 'expect request' ăn mất thì bảng không được
            # gửi lại và node sẽ kẹt).
            for m in list(pending):
                _maybe_repush(m)

            if action == "publish_cmd":
                sim.publish_cmd(step["node"], step["cmd"])
                last_cmd[int(step["node"])] = step["cmd"]
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
                expect_msg = step.get("expect_msg", {})

                # 1) Message đã được đệm từ bước trước có thể đã thoả rồi.
                for i, msg in enumerate(pending):
                    ok, _ = _msg_matches(msg, expect_msg)
                    if ok:
                        pending.pop(i)
                        matched = True
                        print("  ✓ khớp message đệm sẵn: {} {}".format(
                            msg[0], _short(msg[1])))
                        break

                # 2) Chưa có thì chờ message mới; message không khớp thì đệm lại.
                while not matched and time.time() < step_deadline:
                    msg = sim.drain(timeout=min(1.0, max(0, step_deadline - time.time())))
                    if msg is None:
                        continue
                    _maybe_repush(msg)
                    ok, err = _msg_matches(msg, expect_msg)
                    if ok:
                        matched = True
                        print("  ✓ nhận được message thoả: {} {}".format(
                            msg[0], _short(msg[1])))
                        break
                    if len(pending) < PENDING_MAX:
                        pending.append(msg)
                    print("  - đệm lại cho bước sau ({}): {} {}".format(
                        err, msg[0], _short(msg[1])))
                if matched:
                    results.append((step_name, "PASS", True))
                else:
                    results.append((step_name, "FAIL", False))
                continue

            if action == "assert_not_seen":
                # Khẳng định KHÔNG có message nào khớp (đã đệm hoặc mới tới).
                # Dùng để kiểm tra THỨ TỰ, ví dụ: node 2 KHÔNG được gửi 'done'
                # trước khi node 1 hoàn tất (gateway cấp baseline tuần tự).
                topic_re = step.get("topic_regex", "")
                want = step.get("expect", {})

                def _is_seen(m):
                    if topic_re and not re.search(topic_re, m[0]):
                        return False
                    if not want:
                        return True
                    obj = _payload_of(m)
                    return bool(obj) and all(obj.get(k) == v for k, v in want.items())

                bad = [m for m in pending if _is_seen(m)]

                # Drain everything ALREADY queued (non-blocking): một vi phạm đã
                # tới nhưng chưa được bước nào lấy ra vẫn phải bị phát hiện.
                while not bad:
                    msg = sim.drain(timeout=0)
                    if msg is None:
                        break
                    _maybe_repush(msg)
                    if _is_seen(msg):
                        bad.append(msg)
                        break
                    if len(pending) < PENDING_MAX:
                        pending.append(msg)

                # Cửa sổ chờ thêm cho message đang bay tới. Mặc định 0 vì nhiều
                # trường hợp message tới sau là HỢP LỆ (ví dụ node kế tiếp được
                # chuyển lượt ngay sau khi node trước xong).
                settle = step.get("settle_seconds", 0)
                if not bad and settle > 0:
                    settle_deadline = time.time() + settle
                    while time.time() < settle_deadline:
                        msg = sim.drain(timeout=0.5)
                        if msg is None:
                            continue
                        _maybe_repush(msg)
                        if _is_seen(msg):
                            bad.append(msg)
                            break
                        if len(pending) < PENDING_MAX:
                            pending.append(msg)

                if bad:
                    print("  ✗ VI PHẠM THỨ TỰ: đã thấy message không được phép: {} {}".format(
                        bad[0][0], _short(bad[0][1])))
                    results.append((step_name, "FAIL", False))
                else:
                    print("  ✓ không thấy message cấm (đã kiểm tra pending + queue, chờ thêm {}s)".format(
                        settle))
                    results.append((step_name, "PASS", True))
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
        if pending:
            print("  (còn {} message không khớp bước nào - xem log phía trên)".format(
                len(pending)))
    except KeyboardInterrupt:
        # Ctrl+C: dừng gọn, KHÔNG để traceback kép từ cleanup. Bước đang chờ bị
        # đánh dấu INTERRUPTED, các bước đã chạy vẫn được ghi vào báo cáo.
        interrupted = True
        print("\n⏹  DỪNG BỞI NGƯỜI DÙNG (Ctrl+C) khi đang ở bước: {}".format(step_name))
        results.append((step_name + " (bị dừng giữa chừng)", "INTERRUPTED", False))
    finally:
        _safe_stop(sim)

    elapsed = time.time() - start_all
    passed = sum(1 for r in results if r[2])
    print("\n===== KẾT QUẢ {} =====".format("SUITE" if not interrupted else "SUITE (một phần, đã dừng)"))
    print("  Pass: {}/{}".format(passed, len(results)))
    print("  Thời gian: {:.1f}s".format(elapsed))
    for name_, status, ok in results:
        print("    [{}] {}".format("PASS" if ok else "FAIL", name_))
    if pending:
        print("  (còn {} message không khớp bước nào - xem log phía trên)".format(
            len(pending)))

    _write_report(report_dir, suite, results, elapsed=elapsed,
                  leftover=len(pending))
    return (not interrupted) and passed == len(results)


def _safe_stop(sim):
    """Dừng MQTT an toàn.

    `client.loop_stop()` join thread nội bộ của paho; nếu người dùng bấm Ctrl+C
    lần thứ hai (hoặc lúc join) thì chính cleanup lại ném KeyboardInterrupt →
    traceback kép rất khó đọc. Ở đây nuốt mọi lỗi cleanup để luôn thoát sạch.
    """
    for fn_name in ("disconnect", "loop_stop"):
        fn = getattr(sim.client, fn_name, None)
        if fn is None:
            continue
        try:
            fn()
        except BaseException:      # gồm cả KeyboardInterrupt ở lần Ctrl+C thứ 2
            pass


def _short(payload, n=120):
    if isinstance(payload, bytes):
        payload = payload.decode("utf-8", errors="replace")
    return payload if len(payload) <= n else payload[:n] + "..."


def _write_report(report_dir, suite, results, elapsed, leftover=0):
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
        "unmatched_leftover": leftover,
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

def _resolve_suite(arg):
    """
    Tìm file suite theo nhiều cách để khỏi phải gõ đúng đường dẫn:
      baseline_provision             -> <test>/suites/baseline_provision.json
      baseline_provision.json        -> <test>/suites/baseline_provision.json
      suites/baseline_provision.json -> giữ nguyên
    Trả về đường dẫn tồn tại, hoặc None nếu không thấy.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    names = [arg] if arg.endswith(".json") else [arg, arg + ".json"]
    cands = []
    for n in names:
        cands.append(n)
        if not os.path.isabs(n) and os.sep not in n and "/" not in n:
            cands.append(os.path.join("suites", n))
    for c in cands:
        p = c if os.path.isabs(c) else os.path.join(here, c)
        if os.path.isfile(p):
            return p
    return None


def _list_suites():
    """Danh sách các suite có sẵn trong thư mục suites/."""
    d = os.path.join(os.path.dirname(os.path.abspath(__file__)), "suites")
    try:
        return sorted(f for f in os.listdir(d) if f.endswith(".json"))
    except OSError:
        return []


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
    ap.add_argument("--suite", default="",
                    help="Chạy suite JSON (có thể ghi tắt tên file: '--suite baseline_provision')")
    ap.add_argument("--check-schema", choices=list(SCHEMAS.keys()),
                    help="Chờ 1 message và validate schema")
    ap.add_argument("--timeout", type=int, default=30,
                    help="Timeout cho check-schema / suite")
    ap.add_argument("--report-dir", default=os.path.join(os.path.dirname(__file__), "reports"))

    args = ap.parse_args()
    nodes = [int(x) for x in args.nodes.split(",") if x.strip()]

    # Tìm suite TRƯỚC khi kết nối: báo lỗi sớm và tránh mở MQTT vô ích.
    suite_path = ""
    if args.suite:
        suite_path = _resolve_suite(args.suite)
        if not suite_path:
            print("❌ Không tìm thấy suite '{}'".format(args.suite))
            avail = _list_suites()
            if avail:
                print("   Suite có sẵn trong suites/ :")
                for f in avail:
                    print("     - suites/{}".format(f))
            print("   Ví dụ: --suite suites/baseline_provision_new_schema.json")
            sys.exit(2)

    sim = ServerSim(args.broker, args.port, args.user, args.password,
                    args.site, args.gw, nodes=nodes, insecure=args.insecure)

    if not sim.connect(timeout=30):
        sys.exit(1)

    if args.suite:
        try:
            ok = run_suite(sim, suite_path, args.report_dir)
        except KeyboardInterrupt:
            print("\n⏹  Đã dừng bởi người dùng (Ctrl+C).")
            _safe_stop(sim)
            sys.exit(130)
        sys.exit(0 if ok else 1)
    elif args.check_schema:
        try:
            ok = check_schema_mode(sim, args.check_schema, args.timeout)
        except KeyboardInterrupt:
            print("\n⏹  Đã dừng bởi người dùng (Ctrl+C).")
            _safe_stop(sim)
            sys.exit(130)
        sys.exit(0 if ok else 1)
    else:
        try:
            listen_mode(sim)
        except KeyboardInterrupt:
            print("\n⏹  Đã dừng bởi người dùng (Ctrl+C).")
        finally:
            _safe_stop(sim)


if __name__ == "__main__":
    main()
