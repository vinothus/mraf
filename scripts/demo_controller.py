#!/usr/bin/env python3
"""Simulated MRAF controller for the Spring Boot CoAP server demo.

It implements the same polling contract as firmware/mraf/coap_client.cpp:
confirmable POST, JSON, token, next-poll ack and on-demand data snapshots.
"""
import argparse
import copy
import json
import os
import random
import socket
import struct
import time
from datetime import datetime
from pathlib import Path
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parent.parent


def encode_option(number, last, value):
    delta = number - last
    dn = delta if delta < 13 else (13 if delta < 269 else 14)
    ln = len(value) if len(value) < 13 else (13 if len(value) < 269 else 14)
    out = bytearray([(dn << 4) | ln])
    if delta >= 13:
        d = delta - (13 if dn == 13 else 269)
        out += bytes([d]) if dn == 13 else struct.pack(">H", d)
    if len(value) >= 13:
        l = len(value) - (13 if ln == 13 else 269)
        out += bytes([l]) if ln == 13 else struct.pack(">H", l)
    out += value
    return bytes(out)


def build_packet(path, payload):
    mid = random.randint(1, 65535)
    token = os.urandom(4)
    pkt = bytearray([0x44, 0x02, mid >> 8, mid & 0xFF]) + token
    last = 0
    for segment in [s for s in path.split("/") if s]:
        pkt += encode_option(11, last, segment.encode())
        last = 11
    pkt += encode_option(12, last, bytes([50]))
    pkt += b"\xff" + payload.encode()
    return bytes(pkt), mid, token


def decode_packet(data):
    if len(data) < 4:
        raise ValueError("short CoAP packet")
    tkl = data[0] & 0x0F
    code = data[1]
    pos = 4 + tkl
    while pos < len(data):
        b = data[pos]
        pos += 1
        if b == 0xFF:
            return code, data[pos:].decode(errors="replace")
        d, ln = b >> 4, b & 0x0F
        if d == 13:
            pos += 1
        elif d == 14:
            pos += 2
        if ln == 13:
            pos += 1
        elif ln == 14:
            pos += 2
        pos += ln
    return code, ""


def coap_post(host, port, path, payload, timeout=2.0):
    packet, mid, token = build_packet(path, payload)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(timeout)
        sock.sendto(packet, (host, port))
        while True:
            data, _ = sock.recvfrom(8192)
            if len(data) < 4 or (data[0] >> 6) != 1:
                continue
            if data[1] == 0:
                continue
            tkl = data[0] & 0x0F
            if data[4:4 + tkl] != token:
                continue
            return decode_packet(data)


def schedule_active(windows, days, tz):
    now = datetime.now(tz)
    minute = now.hour * 60 + now.minute
    bit = 1 << now.weekday()
    if not days & bit:
        return False
    for w in windows:
        try:
            oh, om = map(int, w["on"].split(":"))
            fh, fm = map(int, w["off"].split(":"))
        except Exception:
            continue
        on, off = oh * 60 + om, fh * 60 + fm
        if on == off:
            continue
        if on < off and on <= minute < off:
            return True
        if on > off and (minute >= on or minute < off):
            return True
    return False


def recurring_action(rule, tz):
    """Return 'on', 'off' or '' for a recurring rule at the current time."""
    now = datetime.now(tz)
    repeat = rule.get("repeat", "daily")
    if repeat == "daily":
        active = True
    elif repeat == "weekly":
        active = bool(int(rule.get("days", 0)) & (1 << now.weekday()))
    elif repeat == "monthly":
        active = now.day == int(rule.get("dom", 1))
    elif repeat == "yearly":
        active = now.month == int(rule.get("month", 1)) and now.day == int(rule.get("dom", 1))
    else:
        active = False
    if not active:
        return ""
    try:
        fh, fm = map(int, rule.get("from", "00:00").split(":"))
        th, tm = map(int, rule.get("to", "00:00").split(":"))
    except Exception:
        return ""
    minute = now.hour * 60 + now.minute
    start, end = fh * 60 + fm, th * 60 + tm
    if start == end:
        covers = False
    elif start < end:
        covers = start <= minute < end
    else:
        covers = minute >= start or minute < end
    return rule.get("action", "on") if covers else ""


def rule_channels(rule):
    channels = rule.get("channels")
    if isinstance(channels, list) and channels:
        return {int(c) for c in channels}
    mask = int(rule.get("channel_mask", 0))
    return {i + 1 for i in range(12) if mask & (1 << i)}


class DemoController:
    def __init__(self, host, port, token, interval,
                 device_id="SIM-000001", user="farm", password="farm1234"):
        self.host = host
        self.port = port
        self.token = token
        self.interval = interval
        self.device_id = device_id
        self.user = user
        self.password = password
        self.confirmed = False
        self.profile = "agri"
        self.poll_interval = 60
        self.tz = ZoneInfo("Asia/Kolkata")
        cfg = json.loads((ROOT / "simulator/config.json").read_text())
        self.channels = cfg["channels"]
        self.pump_cfg = cfg["pump"]
        self.recurring = cfg.get("recurring", [])
        self.relays = [False] * 12
        self.pulse_until = {}
        self.pump_on = False
        self.ack_queue = []
        self.data_request_id = None
        self.battery = 13.32
        self.polls = 0
        self.wireguard = {}
        self.started = time.time()

    def recurring_for(self, index):
        action = ""
        for rule in self.recurring:
            if index + 1 not in rule_channels(rule):
                continue
            result = recurring_action(rule, self.tz)
            if result == "off":
                return "off"
            if result == "on":
                action = "on"
        return action

    def refresh_auto(self):
        now = time.time()
        for i, ch in enumerate(self.channels):
            rec = self.recurring_for(i)
            ch["recurring"] = bool(rec)
            if ch["mode"] == "on":
                wanted = True
            elif ch["mode"] == "off":
                wanted = False
            elif not ch.get("enabled", True):
                wanted = False
            elif rec == "off":
                wanted = False
            elif rec == "on":
                wanted = True
            else:
                wanted = schedule_active(ch.get("windows", []), ch.get("days", 0), self.tz)
            if self.pulse_until.get(i, 0) > now:
                wanted = True
            elif self.pulse_until.get(i, 0):
                self.pulse_until[i] = 0
            self.relays[i] = wanted
        p = self.pump_cfg
        if p["mode"] == "on":
            self.pump_on = True
        elif p["mode"] == "off":
            self.pump_on = False
        elif not p.get("enabled", True):
            self.pump_on = False
        else:
            self.pump_on = schedule_active(p.get("windows", []), p.get("days", 0), self.tz)

    def apply_command(self, cmd):
        target = cmd.get("target", "")
        mode = cmd.get("mode", "auto")
        if target in ("relay", "pulse"):
            i = int(cmd.get("id", 0)) - 1
            if not 0 <= i < 12:
                return "rejected"
            if target == "pulse" and mode == "auto":
                mode = "pulse"
            pulse_ms = int(cmd.get("pulse_ms", 0) or 0)
            if target == "pulse" and pulse_ms <= 0:
                pulse_ms = int(self.channels[i].get("pulse_on_ms", 600))
            self.channels[i]["mode"] = mode
            if pulse_ms > 0 and mode in ("on", "pulse"):
                self.pulse_until[i] = time.time() + max(pulse_ms, 20) / 1000.0
                self.relays[i] = True
            elif mode == "on":
                self.relays[i] = True
            elif mode == "off":
                self.relays[i] = False
                self.pulse_until.pop(i, None)
        elif target == "pump":
            self.pump_cfg["mode"] = mode
            if mode == "on":
                self.pump_on = True
            elif mode == "off":
                self.pump_on = False
        elif target == "all":
            for i, ch in enumerate(self.channels):
                ch["mode"] = mode
                self.relays[i] = mode == "on"
            self.pump_cfg["mode"] = mode
            self.pump_on = mode == "on"
        else:
            return "rejected"
        return "applied"

    def run_console(self, command):
        """Emulate the on-device Zephyr shell for remote console access."""
        line = (command or "").strip()
        if not line:
            return ""
        argv = line.split()
        cmd = argv[0]
        if cmd in ("help", "?"):
            return "\n".join([
                "Please press the <Tab> button to see all available commands.",
                "You can also use the <Tab> button to complete a command.",
                "Available commands:",
                "  clear       : Clear screen.",
                "  device      : Device commands.",
                "  help        : Prints the list of commands.",
                "  kernel      : Kernel commands.",
                "  mraf        : Neer Kaavalan controller commands.",
                "  net         : Networking commands.",
                "  threads     : List kernel threads.",
                "  uptime      : Show system uptime.",
                "  version     : Show version.",
            ])
        if cmd == "clear":
            return ""
        if cmd == "version":
            return "Neer Kaavalan 1.1.0-demo (Zephyr 4.4.0)"
        if cmd == "uptime":
            return f"Uptime: {int(time.time() - self.started)} s"
        if cmd == "threads":
            return "\n".join([
                "Name             Prio State  Stack",
                "mraf_remote         7 running 6144",
                "mraf_state          8 pending 4096",
                "mraf_web            9 pending 8192",
                "sysworkq           -1 pending 2048",
                "idle                15 ready  1024",
            ])
        if cmd == "device":
            sub = argv[1] if len(argv) > 1 else ""
            if sub == "list":
                return "\n".join([
                    "devices:",
                    "- uart@3f400000 (READY)",
                    "- gpio@3ff44000 (READY)",
                    "- adc@3ff48800 (READY)",
                    "- wdt@3ff5a000 (READY)",
                ])
            return "device: subcommand 'list' not found"
        if cmd == "net":
            sub = argv[1] if len(argv) > 1 else ""
            if sub == "iface":
                lines = [
                    "Interface 0x20001234 (wlan0)",
                    "============================",
                    "Link addr : 3C:71:BF:AA:01:42",
                    "MTU       : 1500",
                    "Oper state: UP",
                    "IPv4 addr : 192.168.1.42/24",
                ]
                if self.wireguard:
                    lines += [
                        f"Interface {self.wireguard.get('interface', 'wg0')} (VPN)",
                        "Oper state: UP",
                        f"IPv4 addr : {self.wireguard.get('address', '')}",
                    ]
                return "\n".join(lines)
            return "net: subcommand 'iface' not found"
        if cmd == "kernel":
            sub = " ".join(argv[1:])
            if sub == "version":
                return "Zephyr version 4.4.0"
            return "kernel: subcommand 'version' not found"
        if cmd == "mraf":
            sub = argv[1] if len(argv) > 1 else "status"
            if sub == "status":
                self.refresh_auto()
                on = sum(1 for r in self.relays if r)
                return "\n".join([
                    f"device_id : {self.device_id}",
                    f"name      : Neer Kaavalan",
                    f"profile   : {self.profile}",
                    f"confirmed : {str(self.confirmed).lower()}",
                    f"relays    : {on}/12 on",
                    f"pump      : {'RUNNING' if self.pump_on else 'OFF'} "
                    f"(mode {self.pump_cfg.get('mode', 'auto')})",
                    f"coap      : {'enabled' if self.confirmed else 'pending'}",
                ])
            if sub == "relay":
                if len(argv) < 4:
                    return "usage: mraf relay <1-12> <on|off|auto>"
                try:
                    idx = int(argv[2]) - 1
                except ValueError:
                    return "invalid relay index"
                if not 0 <= idx < 12:
                    return "invalid relay index"
                mode = argv[3]
                if mode not in ("on", "off", "auto"):
                    return "usage: mraf relay <1-12> <on|off|auto>"
                self.apply_command({"target": "relay", "id": idx + 1, "mode": mode})
                state = "on" if self.relays[idx] else "off"
                return f"relay {idx + 1} -> {state} (mode {mode})"
            if sub == "pump":
                if len(argv) < 3:
                    return "usage: mraf pump <on|off|auto>"
                mode = argv[2]
                if mode not in ("on", "off", "auto"):
                    return "usage: mraf pump <on|off|auto>"
                self.apply_command({"target": "pump", "mode": mode})
                return f"pump -> {'on' if self.pump_on else 'off'} (mode {mode})"
            if sub == "wg":
                if not self.wireguard:
                    return "wireguard: not configured"
                return "\n".join([
                    f"interface : {self.wireguard.get('interface', 'wg0')}",
                    f"address   : {self.wireguard.get('address', '')}",
                    f"endpoint  : {self.wireguard.get('endpoint', '')}",
                    f"allowed   : {self.wireguard.get('allowed_ips', '')}",
                ])
            return f"mraf: unknown subcommand '{sub}'"
        return f"Unknown command: {cmd}"

    def apply_config(self, body):
        profile = body.get("profile") or body.get("device", {}).get("profile")
        if profile:
            self.profile = profile
        if isinstance(body.get("channels"), list):
            incoming = {c.get("id"): c for c in body["channels"]}
            for i, ch in enumerate(self.channels):
                if i + 1 in incoming:
                    ch.update(copy.deepcopy(incoming[i + 1]))
        if isinstance(body.get("pump"), dict):
            self.pump_cfg.update(copy.deepcopy(body["pump"]))
        if isinstance(body.get("recurring"), list):
            self.recurring = copy.deepcopy(body["recurring"])
        self.refresh_auto()
        return "applied"

    def snapshot(self):
        self.refresh_auto()
        return {
            "now": int(time.time()),
            "device_id": self.device_id,
            "profile": self.profile,
            "confirmed": self.confirmed,
            "power": True,
            "battery_v": round(self.battery, 2),
            "tank_full": False,
            "sump_dry": False,
            "wifi_rssi": -58,
            "ip": "192.168.1.42",
            "pump": {
                "on": self.pump_on,
                "state": "RUNNING" if self.pump_on else "OFF",
                "mode": self.pump_cfg.get("mode", "auto"),
                "current_a": 6.8 if self.pump_on else 0.0,
                "fault": "",
                "fault_latched": False,
            },
            "relays": [1 if r else 0 for r in self.relays],
            "relay_info": [
                {
                    "id": i + 1,
                    "name": ch.get("name", f"Relay {i + 1}"),
                    "zone": ch.get("zone", ""),
                    "equipment": ch.get("equipment", "other"),
                    "pulse_mode": bool(ch.get("pulse_mode")),
                    "output": bool(self.relays[i]),
                    "recurring": bool(ch.get("recurring")),
                }
                for i, ch in enumerate(self.channels)
            ],
        }

    def apply_meta(self, response):
        if "confirmed" in response:
            self.confirmed = bool(response["confirmed"])
        profile = response.get("profile")
        if profile:
            self.profile = profile
        interval = response.get("poll_interval_sec")
        if isinstance(interval, int) and interval > 0:
            self.poll_interval = interval

    def poll(self):
        self.polls += 1
        request = {
            "device_id": self.device_id,
            "device": "Neer Kaavalan",
            "fw": "1.1.0-demo",
            "token": self.token,
            "time": int(time.time()),
            "profile": self.profile,
            "confirmed": self.confirmed,
            "register": True,
            "poll_interval_sec": self.poll_interval,
            "auth": {"user": self.user, "pass": self.password},
        }
        if self.ack_queue:
            ack = self.ack_queue.pop(0)
            request["ack"] = ack["id"]
            request["result"] = ack["result"]
            if ack["result"] == "data":
                request["data"] = self.snapshot()
            if ack.get("console") is not None:
                request["console"] = ack["console"]

        code, text = coap_post(self.host, self.port, "/mraf", json.dumps(request))
        if not 64 <= code < 96:
            print(f"CoAP response {code}: {text}")
            return
        if not text:
            print("poll ok, empty response")
            return
        response = json.loads(text)
        self.apply_meta(response)
        kind = response.get("request")
        rid = response.get("id", "")
        if kind == "register":
            status = response.get("status", "pending")
            if status == "confirmed":
                self.confirmed = True
                print(f"poll {self.polls}: device confirmed by admin")
            elif status == "rejected":
                self.confirmed = False
                print(f"poll {self.polls}: registration rejected")
            else:
                print(f"poll {self.polls}: registration pending admin confirmation")
        elif kind == "auth":
            self.confirmed = False
            print(f"poll {self.polls}: server login rejected ({response.get('error', 'auth')})")
        elif kind == "control":
            results = [self.apply_command(c) for c in response.get("commands", [])]
            result = "applied" if all(r == "applied" for r in results) else "rejected"
            if rid:
                self.ack_queue.append({"id": rid, "result": result})
            print(f"poll {self.polls}: control {rid} -> {result}")
        elif kind == "data":
            if rid:
                self.ack_queue.append({"id": rid, "result": "data"})
            print(f"poll {self.polls}: data request {rid} queued for next poll")
        elif kind == "config":
            result = self.apply_config(response)
            if rid:
                self.ack_queue.append({"id": rid, "result": result})
            print(f"poll {self.polls}: config {rid} -> {result}")
        elif kind == "console":
            command = response.get("command", "")
            output = self.run_console(command)
            if rid:
                self.ack_queue.append({"id": rid, "result": "ok", "console": output})
            print(f"poll {self.polls}: console {rid} <- {command!r}")
        elif kind == "wireguard":
            peer = response.get("peer") or {}
            self.wireguard = {
                "interface": response.get("interface"),
                "address": response.get("address"),
                "endpoint": peer.get("endpoint"),
                "allowed_ips": peer.get("allowed_ips"),
                "public_key": peer.get("public_key"),
            }
            configured = bool(self.wireguard["address"] and peer.get("public_key"))
            result = "applied" if configured else "rejected"
            if rid:
                self.ack_queue.append({"id": rid, "result": result})
            print(f"poll {self.polls}: wireguard {rid} -> {result}")
        else:
            print(f"poll {self.polls}: none")

    def run(self):
        print(f"demo controller -> coap://{self.host}:{self.port}/mraf")
        while True:
            try:
                self.poll()
            except KeyboardInterrupt:
                return
            except Exception as e:
                print(f"poll failed: {e}")
            time.sleep(self.interval)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5683)
    parser.add_argument("--token", default=os.environ.get("MRAF_TOKEN", "demo-token"))
    parser.add_argument("--interval", type=float, default=2.0)
    parser.add_argument("--device-id", default=os.environ.get("MRAF_DEVICE_ID", "SIM-000001"))
    parser.add_argument("--user", default=os.environ.get("MRAF_USER", "farm"))
    parser.add_argument("--password", default=os.environ.get("MRAF_PASSWORD", "farm1234"))
    args = parser.parse_args()
    DemoController(args.host, args.port, args.token, args.interval,
                   args.device_id, args.user, args.password).run()


if __name__ == "__main__":
    main()
