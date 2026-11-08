#!/usr/bin/env python3
"""MRAF native digital-twin simulator.

Runs the same schedule, pump state machine, interlocks and REST API as the
ESP32 firmware, on this machine, using only the Python standard library.

    python3 simulator/mraf_sim.py --speed 60 --port 8080

The simulator is intentionally a *model*, not an instruction-level emulator.
It lets you exercise schedules, power cuts, float switches and pump faults
without hardware. The ESP32 firmware is the real target.
"""
from __future__ import annotations

import argparse
import base64
import copy
import json
import random
import re
import sys
import threading
import time
from collections import deque
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Optional

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CONFIG = Path(__file__).resolve().parent / "config.json"
WEB_INDEX = ROOT / "web" / "index.html"
FW_VERSION = "1.1.0"
MAX_CHANNELS = 12
MAX_WINDOWS = 4
MAX_RECURRING = 8
EVENT_LOG_SIZE = 200
MODES = ("auto", "on", "off")
PROFILES = ("agri", "home", "godown", "industry")
EQUIPMENT = ("valve", "light", "motor", "fan", "pump", "socket", "heater",
             "door", "other")
REPEATS = ("daily", "weekly", "monthly", "yearly")
ACTIONS = ("on", "off", "auto")


class ConfigError(ValueError):
    pass


# ---------------------------------------------------------------------------
# Time / config helpers
# ---------------------------------------------------------------------------
_TZ_RE = re.compile(r"([A-Za-z]{3,6})([+-])(\d{1,2})(?::(\d{2}))?")


def parse_tz(value: str) -> timezone:
    """Interpret a POSIX TZ string such as IST-5:30 (UTC+05:30)."""
    m = _TZ_RE.fullmatch((value or "").strip())
    if not m:
        return timezone.utc
    numeric = (1 if m.group(2) == "+" else -1) * (
        int(m.group(3)) * 60 + int(m.group(4) or 0)
    )
    # POSIX offset is added to local time to obtain UTC, hence the negation.
    off_min = -numeric
    name = m.group(1)
    return timezone(timedelta(minutes=off_min), name)


def parse_hhmm(s: Any) -> int:
    if not isinstance(s, str):
        raise ConfigError("time must be a HH:MM string")
    m = re.fullmatch(r"(\d{1,2}):(\d{2})", s.strip())
    if not m:
        raise ConfigError(f"invalid time '{s}' (use HH:MM)")
    h, mi = int(m.group(1)), int(m.group(2))
    if h > 23 or mi > 59:
        raise ConfigError(f"invalid time '{s}'")
    return h * 60 + mi


def fmt_hhmm(minutes: int) -> str:
    return f"{minutes // 60:02d}:{minutes % 60:02d}"


def _merge_section(out: dict, incoming: dict, key: str) -> None:
    if isinstance(incoming.get(key), dict):
        out.setdefault(key, {}).update(incoming[key])


def merge_config(base: dict, incoming: dict) -> dict:
    """Partial merge, mirroring the firmware's per-field update behaviour."""
    if not isinstance(incoming, dict):
        raise ConfigError("config must be a JSON object")
    out = copy.deepcopy(base)
    for key in ("device", "wifi", "coap", "safety"):
        _merge_section(out, incoming, key)

    chans = incoming.get("channels")
    if chans is not None:
        if not isinstance(chans, list):
            raise ConfigError("channels must be an array")
        if len(chans) > MAX_CHANNELS:
            raise ConfigError(f"too many channels (max {MAX_CHANNELS})")
        out.setdefault("channels", [])
        for i, ch in enumerate(chans):
            if not isinstance(ch, dict):
                raise ConfigError("each channel must be an object")
            while len(out["channels"]) <= i:
                out["channels"].append({})
            merged = copy.deepcopy(out["channels"][i])
            merged.update(ch)
            out["channels"][i] = merged

    rec = incoming.get("recurring")
    if rec is not None:
        if not isinstance(rec, list):
            raise ConfigError("recurring must be an array")
        out["recurring"] = copy.deepcopy(rec)

    pump = incoming.get("pump")
    if isinstance(pump, dict):
        out.setdefault("pump", {}).update(pump)
    return out


def _windows(value: Any, label: str) -> list[dict]:
    if not isinstance(value, list):
        raise ConfigError(f"{label}: windows must be an array")
    if len(value) > MAX_WINDOWS:
        raise ConfigError(f"{label}: too many windows (max {MAX_WINDOWS})")
    out = []
    for w in value:
        if not isinstance(w, dict):
            raise ConfigError(f"{label}: invalid window")
        on = parse_hhmm(w.get("on"))
        off = parse_hhmm(w.get("off"))
        out.append({"on": fmt_hhmm(on), "off": fmt_hhmm(off)})
    return out


def _int(value: Any, default: int, lo: int, hi: int, label: str) -> int:
    try:
        v = int(value if value is not None else default)
    except (TypeError, ValueError):
        raise ConfigError(f"{label} must be an integer")
    if v < lo or v > hi:
        raise ConfigError(f"{label} must be {lo}..{hi}")
    return v


def _float(value: Any, default: float, lo: float, hi: float, label: str) -> float:
    try:
        v = float(value if value is not None else default)
    except (TypeError, ValueError):
        raise ConfigError(f"{label} must be a number")
    if v < lo or v > hi:
        raise ConfigError(f"{label} must be {lo}..{hi}")
    return v


def _recurring(value: Any) -> list[dict]:
    if not isinstance(value, list):
        raise ConfigError("recurring must be an array")
    if len(value) > MAX_RECURRING:
        raise ConfigError(f"recurring: too many schedules (max {MAX_RECURRING})")
    out = []
    for i, rs in enumerate(value):
        if not isinstance(rs, dict):
            raise ConfigError("recurring: each schedule must be an object")
        label = f"recurring {i + 1}"
        repeat = str(rs.get("repeat") or "daily")
        if repeat not in REPEATS:
            raise ConfigError(f"{label}: repeat must be daily/weekly/monthly/yearly")
        action = str(rs.get("action") or "on")
        if action not in ACTIONS:
            raise ConfigError(f"{label}: action must be on/off/auto")
        mask = _int(rs.get("channel_mask"), 0, 0, 0x0FFF, f"{label}: channel_mask")
        chans = rs.get("channels")
        if isinstance(chans, list):
            for c in chans:
                cid = _int(c, 0, 1, MAX_CHANNELS, f"{label}: channel id")
                mask |= 1 << (cid - 1)
        out.append({
            "name": str(rs.get("name") or "Schedule")[:23],
            "enabled": bool(rs.get("enabled", True)),
            "repeat": repeat,
            "days": _int(rs.get("days"), 127, 0, 127, f"{label}: days"),
            "day_of_month": _int(rs.get("day_of_month"), 1, 1, 31,
                                 f"{label}: day_of_month"),
            "month": _int(rs.get("month"), 1, 1, 12, f"{label}: month"),
            "from": fmt_hhmm(parse_hhmm(rs.get("from") or "06:00")),
            "to": fmt_hhmm(parse_hhmm(rs.get("to") or "07:00")),
            "channel_mask": mask,
            "action": action,
        })
    return out


def normalize_config(cfg: dict) -> dict:
    if not isinstance(cfg, dict):
        raise ConfigError("config must be an object")
    out = copy.deepcopy(cfg)
    out.setdefault("device", {})
    out.setdefault("wifi", {})
    out.setdefault("coap", {})
    out.setdefault("safety", {})
    out.setdefault("pump", {})
    out.setdefault("channels", [])
    if not isinstance(out["channels"], list):
        raise ConfigError("channels must be an array")
    if len(out["channels"]) > MAX_CHANNELS:
        raise ConfigError(f"too many channels (max {MAX_CHANNELS})")

    while len(out["channels"]) < MAX_CHANNELS:
        i = len(out["channels"])
        out["channels"].append(
            {"id": i + 1, "name": f"Relay {i + 1}", "zone": f"Zone {i + 1}",
             "equipment": "other", "pulse_mode": False, "enabled": False,
             "days": 0, "mode": "auto", "windows": []}
        )

    for i, ch in enumerate(out["channels"]):
        if not isinstance(ch, dict):
            raise ConfigError(f"channel {i + 1} must be an object")
        label = f"channel {i + 1}"
        ch["id"] = i + 1
        ch["name"] = str(ch.get("name") or f"Relay {i + 1}")[:23]
        ch["zone"] = str(ch.get("zone") or f"Zone {i + 1}")[:23]
        ch["equipment"] = str(ch.get("equipment") or "other")
        if ch["equipment"] not in EQUIPMENT:
            raise ConfigError(f"{label}: unknown equipment")
        ch["pulse_mode"] = bool(ch.get("pulse_mode", False))
        ch["pulse_on_ms"] = _int(ch.get("pulse_on_ms"), 600, 20, 10000,
                                 f"{label}: pulse_on_ms")
        ch["pulse_off_ms"] = _int(ch.get("pulse_off_ms"), 600, 20, 10000,
                                  f"{label}: pulse_off_ms")
        ch["enabled"] = bool(ch.get("enabled", False))
        ch["days"] = _int(ch.get("days"), 0, 0, 127, f"{label}: days")
        ch["mode"] = str(ch.get("mode", "auto"))
        if ch["mode"] not in MODES:
            raise ConfigError(f"{label}: mode must be auto/on/off")
        ch["windows"] = _windows(ch.get("windows", []), label)

    p = out["pump"]
    if not isinstance(p, dict):
        raise ConfigError("pump must be an object")
    p["name"] = str(p.get("name") or "5HP Pump")[:23]
    p["enabled"] = bool(p.get("enabled", True))
    p["days"] = _int(p.get("days"), 127, 0, 127, "pump: days")
    p["mode"] = str(p.get("mode", "auto"))
    if p["mode"] not in MODES:
        raise ConfigError("pump: mode must be auto/on/off")
    p["windows"] = _windows(p.get("windows", []), "pump")
    p["min_off_sec"] = _int(p.get("min_off_sec"), 300, 30, 3600, "pump: min_off_sec")
    p["max_run_min"] = _int(p.get("max_run_min"), 120, 1, 720, "pump: max_run_min")
    p["recovery_delay_sec"] = _int(
        p.get("recovery_delay_sec"), 30, 0, 600, "pump: recovery_delay_sec"
    )
    p["resume_after_outage"] = bool(p.get("resume_after_outage", True))
    p["dry_run_current_a"] = _float(
        p.get("dry_run_current_a"), 1.5, 0.5, 20.0, "pump: dry_run_current_a"
    )
    p["dry_run_grace_sec"] = _int(
        p.get("dry_run_grace_sec"), 20, 5, 120, "pump: dry_run_grace_sec"
    )
    p["tank_full_active_low"] = bool(p.get("tank_full_active_low", True))
    p["sump_dry_active_low"] = bool(p.get("sump_dry_active_low", True))

    out["safety"]["max_boost_min"] = _int(
        out["safety"].get("max_boost_min"), 180, 1, 1440, "safety: max_boost_min"
    )
    dev = out["device"]
    dev["name"] = str(dev.get("name") or "MRAF Farm Controller")[:39]
    dev["timezone"] = str(dev.get("timezone") or "IST-5:30")[:15]
    dev["auth_user"] = str(dev.get("auth_user") or "admin")[:19]
    dev["auth_pass"] = str(dev.get("auth_pass") or "farm1234")[:31]
    dev["device_id"] = str(dev.get("device_id") or "")[:23]
    dev["profile"] = str(dev.get("profile") or "agri")
    if dev["profile"] not in PROFILES:
        raise ConfigError("device: profile must be agri/home/godown/industry")
    out["wifi"]["ssid"] = str(out["wifi"].get("ssid") or "")[:32]
    out["wifi"]["password"] = str(out["wifi"].get("password") or "")[:64]
    out["wifi"]["hostname"] = str(out["wifi"].get("hostname") or "mraf")[:32]
    coap = out["coap"]
    coap["enabled"] = bool(coap.get("enabled", False))
    coap["host"] = str(coap.get("host") or "")[:63]
    coap["port"] = _int(coap.get("port"), 5683, 1, 65535, "coap: port")
    coap["path"] = str(coap.get("path") or "/mraf")[:31]
    if not coap["path"].startswith("/"):
        raise ConfigError("coap: path must start with /")
    coap["interval_sec"] = _int(
        coap.get("interval_sec"), 60, 10, 3600, "coap: interval_sec"
    )
    coap["token"] = str(coap.get("token") or "")[:47]
    coap["user"] = str(coap.get("user") or "farm")[:19]
    coap["pass"] = str(coap.get("pass") or "farm1234")[:31]
    coap["register"] = bool(coap.get("register", True))
    if coap["enabled"] and not coap["host"]:
        raise ConfigError("coap: host required when enabled")

    out["recurring"] = _recurring(out.get("recurring") or [])
    return out


def public_config(cfg: dict) -> dict:
    out = copy.deepcopy(cfg)
    out.get("device", {}).pop("auth_pass", None)
    out.get("wifi", {}).pop("password", None)
    out.get("coap", {}).pop("token", None)
    out.get("coap", {}).pop("pass", None)
    out.pop("sim", None)
    return out


def load_config(path: Path) -> dict:
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    return normalize_config(data)


# ---------------------------------------------------------------------------
# Controller (same rules as firmware/mraf/controller.cpp)
# ---------------------------------------------------------------------------
@dataclass
class RelayRT:
    on: bool = False            # logical state (equipment running)
    output: bool = False        # physical relay coil state
    schedule: bool = False
    recurring: bool = False
    pulsing: bool = False
    pulse_until: float = 0.0    # epoch seconds
    boost_until: float = 0.0
    last_change: float = 0.0


@dataclass
class PumpRT:
    on: bool = False
    want: bool = False
    schedule: bool = False
    fault_latched: bool = False
    fault: str = ""
    external_run: bool = False
    need_edge: bool = False
    tank_full: bool = False
    sump_dry: bool = False
    boost_until: float = 0.0
    run_start: float = 0.0
    last_off: float = 0.0
    last_change: float = 0.0
    current_a: float = 0.0


class FarmController:
    def __init__(self, cfg: dict, tz: timezone):
        self.cfg = copy.deepcopy(cfg)
        self.tz = tz
        self.relays = [RelayRT() for _ in range(MAX_CHANNELS)]
        self.pump = PumpRT()
        self.events: deque[dict] = deque(maxlen=EVENT_LOG_SIZE)
        self.power = True
        self.outage_start = 0.0
        self.restored_at = 0.0
        self.battery_v = 0.0
        self.last_batt_log = 0.0
        self._prepare()

    def _prepare(self) -> None:
        self.ch_windows = [
            [(parse_hhmm(w["on"]), parse_hhmm(w["off"])) for w in ch["windows"]]
            for ch in self.cfg["channels"]
        ]
        self.pump_windows = [
            (parse_hhmm(w["on"]), parse_hhmm(w["off"]))
            for w in self.cfg["pump"]["windows"]
        ]

    def apply_config(self, cfg: dict) -> None:
        self.cfg = copy.deepcopy(cfg)
        self._prepare()

    # -- events -------------------------------------------------------------
    def add_event(self, src: str, msg: str, now: float) -> None:
        self.events.append({"t": float(now), "src": src, "msg": msg})

    # -- schedules ----------------------------------------------------------
    @staticmethod
    def _schedule_active(windows, days: int, weekday: int, now_min: int) -> bool:
        for on, off in windows:
            if on == off:
                continue
            if on < off:
                if (days & (1 << weekday)) and on <= now_min < off:
                    return True
            else:
                if now_min >= on and (days & (1 << weekday)):
                    return True
                prev = (weekday - 1) % 7
                if now_min < off and (days & (1 << prev)):
                    return True
        return False

    @staticmethod
    def _recurring_date_matches(repeat: str, days: int, dom: int, month: int,
                                dt: datetime) -> bool:
        if repeat == "daily":
            return True
        if repeat == "weekly":
            return bool(days & (1 << dt.weekday()))
        if repeat == "monthly":
            return dt.day == dom
        if repeat == "yearly":
            return dt.month == month and dt.day == dom
        return False

    def _recurring_covers(self, rs: dict, today: datetime, yesterday: datetime,
                          now_min: int) -> bool:
        frm = parse_hhmm(rs["from"])
        to = parse_hhmm(rs["to"])
        if frm == to:
            return False
        rep, days = rs["repeat"], rs["days"]
        dom, month = rs["day_of_month"], rs["month"]
        if frm < to:
            return (self._recurring_date_matches(rep, days, dom, month, today)
                    and frm <= now_min < to)
        if now_min >= frm and self._recurring_date_matches(
                rep, days, dom, month, today):
            return True
        if now_min < to and self._recurring_date_matches(
                rep, days, dom, month, yesterday):
            return True
        return False

    def _recurring_action(self, i: int, now: float) -> Optional[int]:
        """1 = force on, 0 = force off, None = no recurring schedule."""
        today = datetime.fromtimestamp(now, self.tz)
        yesterday = datetime.fromtimestamp(now - 86400, self.tz)
        now_min = today.hour * 60 + today.minute
        off = on = False
        for rs in self.cfg.get("recurring", []):
            if not rs["enabled"]:
                continue
            if not (rs["channel_mask"] & (1 << i)):
                continue
            if not self._recurring_covers(rs, today, yesterday, now_min):
                continue
            if rs["action"] == "on":
                on = True
            elif rs["action"] == "off":
                off = True
        if off:
            return 0
        if on:
            return 1
        return None

    def _relay_wanted(self, i: int, power: bool, now_min: int,
                      weekday: int, now: float) -> bool:
        ch = self.cfg["channels"][i]
        rec = self._recurring_action(i, now)
        self.relays[i].recurring = rec is not None
        if not power:
            return False
        if ch["mode"] == "on":
            return True
        if ch["mode"] == "off":
            return False
        if rec == 0:
            return False
        if rec == 1:
            return True
        sched = bool(ch["enabled"]) and self._schedule_active(
            self.ch_windows[i], ch["days"], weekday, now_min
        )
        return sched or self.relays[i].boost_until > now

    def _start_pulse(self, i: int, ms: int, now: float) -> None:
        rt = self.relays[i]
        rt.pulsing = True
        rt.output = True
        rt.pulse_until = now + max(20, int(ms)) / 1000.0

    def _set_relay_logical(self, i: int, on: bool, now: float) -> None:
        rt = self.relays[i]
        ch = self.cfg["channels"][i]
        rt.on = on
        rt.last_change = now
        if ch["pulse_mode"]:
            ms = ch["pulse_on_ms"] if on else ch["pulse_off_ms"]
            self._start_pulse(i, ms, now)
        else:
            rt.output = on

    def service_pulses(self, now: float) -> None:
        for rt in self.relays:
            if rt.pulsing and now >= rt.pulse_until:
                rt.pulsing = False
                rt.output = False

    def _pump_wanted(self, power: bool, tank_full: bool, sump_dry: bool,
                     now_min: int, weekday: int, now: float) -> bool:
        p = self.cfg["pump"]
        if not power or self.pump.fault_latched:
            return False
        if not p["enabled"] or tank_full or sump_dry:
            return False
        if p["mode"] == "on":
            want = True
        elif p["mode"] == "off":
            want = False
        else:
            sched = self._schedule_active(
                self.pump_windows, p["days"], weekday, now_min
            )
            want = sched or self.pump.boost_until > now
        if want and not p["resume_after_outage"] and self.pump.need_edge \
                and p["mode"] == "auto":
            want = False
        return want

    # -- tick ---------------------------------------------------------------
    def tick(self, now: float, power: bool, tank_full: bool, sump_dry: bool,
             current_a: float, battery_v: float) -> None:
        was_power = self.power
        was_tank = self.pump.tank_full
        was_sump = self.pump.sump_dry
        was_external = self.pump.external_run

        self.battery_v = battery_v
        if 2.0 < battery_v < 12.0 and (
            self.last_batt_log == 0 or now - self.last_batt_log >= 600
        ):
            self.add_event("power", "battery low", now)
            self.last_batt_log = now

        if power and not was_power:
            self.restored_at = now
            self.add_event("power", "mains restored", now)
        elif not power and was_power:
            self.outage_start = now
            self.add_event("power", "mains lost", now)
            if not self.cfg["pump"]["resume_after_outage"]:
                self.pump.need_edge = True
        self.power = power

        if tank_full != was_tank:
            self.add_event("sensor", "tank full" if tank_full else "tank clear", now)
        if sump_dry != was_sump:
            self.add_event("sensor", "sump dry" if sump_dry else "sump ok", now)
        self.pump.tank_full = tank_full
        self.pump.sump_dry = sump_dry

        measured = max(0.0, float(current_a))
        self.pump.current_a = self.pump.current_a * 0.5 + measured * 0.5
        if self.pump.current_a < 0:
            self.pump.current_a = 0.0

        dt = datetime.fromtimestamp(now, self.tz)
        weekday = dt.weekday()          # Monday = 0
        now_min = dt.hour * 60 + dt.minute

        self.service_pulses(now)

        # 12 relays
        for i, ch in enumerate(self.cfg["channels"]):
            rt = self.relays[i]
            if rt.boost_until and now >= rt.boost_until:
                rt.boost_until = 0.0
            rt.schedule = bool(ch["enabled"]) and self._schedule_active(
                self.ch_windows[i], ch["days"], weekday, now_min
            )
            if not power:
                if rt.on or rt.output:
                    rt.on = False
                    rt.output = False
                    rt.pulsing = False
                    rt.last_change = now
                rt.recurring = False
                continue
            if ch["mode"] == "off":
                rt.boost_until = 0.0
            want = self._relay_wanted(i, power, now_min, weekday, now)
            if want != rt.on:
                self._set_relay_logical(i, want, now)
                reason = ("boost" if rt.boost_until > now else
                          ("manual" if ch["mode"] == "on" else
                           ("recurring" if rt.recurring else "schedule")))
                self.add_event(
                    "relay",
                    f"R{i + 1} {'ON' if want else 'OFF'}"
                    f"{' (pulse)' if ch['pulse_mode'] else ''} ({reason})",
                    now,
                )

        # pump
        p = self.cfg["pump"]
        pr = self.pump
        if pr.boost_until and now >= pr.boost_until:
            pr.boost_until = 0.0
        pr.schedule = bool(p["enabled"]) and self._schedule_active(
            self.pump_windows, p["days"], weekday, now_min
        )
        want = self._pump_wanted(power, tank_full, sump_dry, now_min, weekday, now)
        pr.want = want
        flow = pr.current_a >= 0.5

        if not power:
            if pr.on:
                pr.on = False
                pr.last_change = now
            pr.last_off = now
            pr.external_run = False
        elif pr.fault_latched:
            if pr.on:
                pr.on = False
                pr.last_change = now
            pr.last_off = now
        else:
            if was_external and not flow:
                pr.external_run = False
                pr.last_off = now
                self.add_event("pump", "external run ended", now)
            elif not was_external and flow and not pr.on:
                pr.external_run = True
                self.add_event("pump", "external run detected (HAND?)", now)

            if want:
                if not pr.on and not pr.external_run:
                    min_ok = pr.last_off == 0 or now - pr.last_off >= p["min_off_sec"]
                    rec_ok = self.restored_at == 0 or \
                        now - self.restored_at >= p["recovery_delay_sec"]
                    edge_ok = not (
                        not p["resume_after_outage"]
                        and pr.need_edge
                        and p["mode"] == "auto"
                    )
                    if not pr.schedule:
                        pr.need_edge = False
                    if p["mode"] == "on":
                        pr.need_edge = False
                    if min_ok and rec_ok and edge_ok:
                        pr.on = True
                        pr.last_change = now
                        pr.run_start = now
                        self.add_event("pump", "started", now)
            else:
                if pr.on:
                    reason = ("tank full" if tank_full else
                              ("sump dry" if sump_dry else "schedule"))
                    pr.on = False
                    pr.last_change = now
                    pr.last_off = now
                    self.add_event("pump", f"stopped ({reason})", now)

            if pr.on:
                run = now - pr.run_start
                if run > p["max_run_min"] * 60:
                    self._trip("FAULT: max run time", now)
                elif run > p["dry_run_grace_sec"] and \
                        pr.current_a < p["dry_run_current_a"]:
                    self._trip("FAULT: dry run / no current", now)

        if not pr.schedule:
            pr.need_edge = False

    def _trip(self, reason: str, now: float) -> None:
        if self.pump.on:
            self.pump.on = False
        self.pump.last_off = now
        self.pump.last_change = now
        self.pump.fault_latched = True
        self.pump.fault = reason
        self.add_event("pump", reason, now)

    # -- user controls ------------------------------------------------------
    def set_relay_mode(self, index: int, mode: str, boost_min: int, now: float,
                       pulse_ms: int = 0) -> None:
        if not 0 <= index < MAX_CHANNELS:
            raise ConfigError("relay id must be 1..12")
        if mode not in MODES:
            raise ConfigError("mode must be auto/on/off")
        ch = self.cfg["channels"][index]
        rt = self.relays[index]
        if boost_min > 0 and mode != "off":
            boost_min = min(boost_min, self.cfg["safety"]["max_boost_min"])
            ch["mode"] = "auto"
            rt.boost_until = now + boost_min * 60
            self.add_event("control", f"R{index + 1} boost {boost_min}min", now)
        else:
            ch["mode"] = mode
            rt.boost_until = 0.0
            if ch["pulse_mode"]:
                if mode == "on":
                    self._set_relay_logical(index, True, now)
                    if pulse_ms:
                        rt.pulse_until = now + max(20, pulse_ms) / 1000.0
                elif mode == "off" and rt.on:
                    self._set_relay_logical(index, False, now)
                    if pulse_ms:
                        rt.pulse_until = now + max(20, pulse_ms) / 1000.0
                elif mode == "off":
                    rt.on = False
                    rt.output = False
                    rt.pulsing = False
            elif mode == "off":
                rt.on = False
                rt.output = False
                rt.pulsing = False
            self.add_event(
                "control",
                f"R{index + 1} mode {mode}{' (pulse)' if ch['pulse_mode'] else ''}",
                now,
            )

    def set_pump_mode(self, mode: str, boost_min: int, now: float) -> None:
        if mode not in MODES:
            raise ConfigError("mode must be auto/on/off")
        p = self.cfg["pump"]
        pr = self.pump
        if boost_min > 0 and mode != "off":
            boost_min = min(boost_min, self.cfg["safety"]["max_boost_min"])
            p["mode"] = "auto"
            pr.boost_until = now + boost_min * 60
            self.add_event("control", f"pump boost {boost_min}min", now)
        else:
            p["mode"] = mode
            pr.boost_until = 0.0
            if mode == "on":
                pr.need_edge = False
            if mode == "off" and pr.on:
                pr.on = False
                pr.last_change = now
                pr.last_off = now
            self.add_event("control", f"pump mode {mode}", now)

    def all_off(self, now: float) -> None:
        for i, ch in enumerate(self.cfg["channels"]):
            ch["mode"] = "off"
            rt = self.relays[i]
            rt.boost_until = 0.0
            rt.on = False
            rt.output = False
            rt.pulsing = False
            rt.last_change = now
        self.cfg["pump"]["mode"] = "off"
        self.pump.boost_until = 0.0
        if self.pump.on:
            self.pump.on = False
            self.pump.last_change = now
            self.pump.last_off = now
        self.add_event("control", "ALL OFF", now)

    def all_auto(self, now: float) -> None:
        for i, ch in enumerate(self.cfg["channels"]):
            ch["mode"] = "auto"
            self.relays[i].boost_until = 0.0
        self.cfg["pump"]["mode"] = "auto"
        self.pump.boost_until = 0.0
        self.pump.need_edge = False
        self.add_event("control", "ALL AUTO", now)

    def reset_pump_fault(self, now: float) -> None:
        self.pump.fault_latched = False
        self.pump.fault = ""
        self.pump.need_edge = False
        self.add_event("control", "pump fault reset", now)

    # -- views --------------------------------------------------------------
    def outage_seconds(self, now: float) -> float:
        if self.power or self.outage_start == 0:
            return 0.0
        return max(0.0, now - self.outage_start)

    def pump_state_name(self, now: float) -> str:
        pr = self.pump
        p = self.cfg["pump"]
        if pr.fault_latched:
            return "FAULT"
        if not self.power:
            return "NO POWER"
        if pr.external_run:
            return "EXTERNAL"
        if pr.on:
            run = now - pr.run_start
            if run <= p["dry_run_grace_sec"] and \
                    pr.current_a < p["dry_run_current_a"]:
                return "STARTING"
            return "RUNNING"
        if pr.want:
            return "WAITING"
        return "IDLE"


# ---------------------------------------------------------------------------
# Simulated clock / app
# ---------------------------------------------------------------------------
class SimClock:
    def __init__(self, epoch: float, speed: float):
        self._base_epoch = float(epoch)
        self._base_mono = time.monotonic()
        self._speed = float(speed)
        self._lock = threading.Lock()

    def _now_locked(self) -> float:
        return self._base_epoch + (time.monotonic() - self._base_mono) * self._speed

    def now(self) -> float:
        with self._lock:
            return self._now_locked()

    def speed(self) -> float:
        with self._lock:
            return self._speed

    def set_speed(self, speed: float) -> None:
        with self._lock:
            self._base_epoch = self._now_locked()
            self._base_mono = time.monotonic()
            self._speed = max(0.01, float(speed))

    def set_epoch(self, epoch: float) -> None:
        with self._lock:
            self._base_epoch = float(epoch)
            self._base_mono = time.monotonic()

    def step(self, seconds: float) -> None:
        with self._lock:
            self._base_epoch += float(seconds)


class SimApp:
    def __init__(self, cfg: dict, speed: float = 60.0, auth: Optional[tuple[str, str]] = None,
                 index_html: Optional[str] = None, host: str = "0.0.0.0",
                 port: int = 8080, verbose: bool = False):
        self.cfg = normalize_config(cfg)
        self.tz = parse_tz(self.cfg["device"]["timezone"])
        self.controller = FarmController(self.cfg, self.tz)
        self.clock = SimClock(time.time(), speed)
        sim = self.cfg.get("sim", {}) if isinstance(self.cfg.get("sim"), dict) else {}
        self.sim = {
            "mains": bool(sim.get("mains", True)),
            "tank_full": bool(sim.get("tank_full", False)),
            "sump_dry": bool(sim.get("sump_dry", False)),
            "battery_v": float(sim.get("battery_v", 13.2)),
            "current_override": sim.get("current_override"),
            "speed": float(speed),
            "pump_running_current_a": float(sim.get("pump_running_current_a", 6.8)),
        }
        self.auth = auth
        self.verbose = verbose
        self.index_html = index_html or (
            WEB_INDEX.read_text(encoding="utf-8") if WEB_INDEX.exists()
            else "<!doctype html><h1>MRAF simulator</h1>"
        )
        self.host = host
        self.port = port
        self.lock = threading.RLock()
        self._sim_cursor = self.clock.now()
        self._last_tick_t = self._sim_cursor
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self.start_mono = time.monotonic()
        self.controller.add_event("system", "boot", self._sim_cursor)

    # -- simulation loop ----------------------------------------------------
    def _current_locked(self) -> float:
        if self.sim["current_override"] is not None:
            return max(0.0, float(self.sim["current_override"]))
        if (self.controller.pump.on and self.sim["mains"]
                and not self.sim["tank_full"] and not self.sim["sump_dry"]):
            return self.sim["pump_running_current_a"] + random.uniform(-0.03, 0.03)
        return 0.0

    def _tick_once_locked(self, t: float) -> None:
        # Battery is a simple model: charge when mains is present, drain slowly
        # during an outage. The controller only needs the voltage for warnings.
        if self.sim["mains"]:
            self.sim["battery_v"] = min(13.6, self.sim["battery_v"] + 0.01)
        else:
            dt = max(0.0, t - self._last_tick_t)
            self.sim["battery_v"] = max(11.0, self.sim["battery_v"] - dt * 0.08 / 3600.0)
        current = self._current_locked()
        self.controller.tick(
            t, self.sim["mains"], self.sim["tank_full"], self.sim["sump_dry"],
            current, self.sim["battery_v"],
        )
        self._last_tick_t = t

    def _advance_locked(self, target: float) -> None:
        if target < self._sim_cursor:
            self._sim_cursor = target
            self._last_tick_t = target
            return
        max_step = 1.0 if self.sim["speed"] <= 300 else 5.0
        while self._sim_cursor < target:
            step = min(max_step, target - self._sim_cursor)
            self._sim_cursor += step
            self._tick_once_locked(self._sim_cursor)

    def _run(self) -> None:
        while not self._stop.is_set():
            with self.lock:
                self._advance_locked(self.clock.now())
            self._stop.wait(0.2)

    def start(self) -> None:
        if self._thread and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="mraf-sim", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2.0)

    # -- status / config ----------------------------------------------------
    def _status_locked(self, now: float) -> dict:
        self.controller.service_pulses(now)
        dt = datetime.fromtimestamp(now, self.tz)
        wd_names = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"]
        relays = []
        for i, ch in enumerate(self.cfg["channels"]):
            rt = self.controller.relays[i]
            relays.append({
                "id": i + 1,
                "name": ch["name"],
                "zone": ch["zone"],
                "equipment": ch["equipment"],
                "pulse_mode": ch["pulse_mode"],
                "pulse_on_ms": ch["pulse_on_ms"],
                "pulse_off_ms": ch["pulse_off_ms"],
                "enabled": ch["enabled"],
                "days": ch["days"],
                "mode": ch["mode"],
                "on": rt.on,
                "output": rt.output,
                "pulsing": rt.pulsing,
                "schedule": rt.schedule,
                "recurring": rt.recurring,
                "boost_until": int(rt.boost_until),
                "last_change": int(rt.last_change),
                "windows": copy.deepcopy(ch["windows"]),
            })
        p = self.cfg["pump"]
        pr = self.controller.pump
        events = []
        for e in list(self.controller.events)[-30:]:
            events.append({
                "t": int(e["t"]),
                "time": datetime.fromtimestamp(e["t"], self.tz).strftime("%Y-%m-%d %H:%M:%S"),
                "src": e["src"],
                "msg": e["msg"],
            })
        return {
            "simulator": True,
            "fw": FW_VERSION,
            "device": self.cfg["device"]["name"],
            "device_id": self.cfg["device"]["device_id"],
            "profile": self.cfg["device"]["profile"],
            "now": int(now),
            "now_local": dt.strftime("%Y-%m-%d %H:%M:%S"),
            "time_valid": now > 1700000000,
            "uptime_sec": int(time.monotonic() - self.start_mono),
            "wifi": {
                "mode": "sta",
                "ssid": "simulated",
                "ip": "127.0.0.1",
                "rssi": -50,
                "hostname": self.cfg["wifi"]["hostname"],
                "connected": True,
            },
            "remote": {
                "enabled": self.cfg["coap"]["enabled"],
                "host": self.cfg["coap"]["host"],
                "port": self.cfg["coap"]["port"],
                "interval_sec": self.cfg["coap"]["interval_sec"],
                "user": self.cfg["coap"]["user"],
                "device_id": self.cfg["device"]["device_id"],
                "status": "off" if not self.cfg["coap"]["enabled"] else "waiting",
                "confirmed": False,
                "last_ok": False,
                "last_error": "simulator does not poll CoAP" if self.cfg["coap"]["enabled"] else "",
                "last_poll": 0,
                "last_code": 0,
            },
            "power": {
                "present": self.sim["mains"],
                "battery_v": round(self.sim["battery_v"], 2),
                "outage_sec": int(self.controller.outage_seconds(now)),
                "battery_backed": True,
            },
            "sensors": {
                "tank_full": self.sim["tank_full"],
                "sump_dry": self.sim["sump_dry"],
                "acs_zero_mv": 1250.0,
            },
            "relays": relays,
            "pump": {
                "name": p["name"],
                "enabled": p["enabled"],
                "days": p["days"],
                "mode": p["mode"],
                "on": pr.on,
                "want": pr.want,
                "schedule": pr.schedule,
                "boost_until": int(pr.boost_until),
                "state": self.controller.pump_state_name(now),
                "fault": pr.fault,
                "fault_latched": pr.fault_latched,
                "current_a": round(pr.current_a, 2),
                "run_sec": int(now - pr.run_start) if pr.on else 0,
                "last_off": int(pr.last_off),
                "last_change": int(pr.last_change),
                "external_run": pr.external_run,
                "tank_full": pr.tank_full,
                "sump_dry": pr.sump_dry,
                "min_off_sec": p["min_off_sec"],
                "max_run_min": p["max_run_min"],
                "recovery_delay_sec": p["recovery_delay_sec"],
                "resume_after_outage": p["resume_after_outage"],
                "dry_run_current_a": p["dry_run_current_a"],
                "dry_run_grace_sec": p["dry_run_grace_sec"],
                "windows": copy.deepcopy(p["windows"]),
            },
            "events": events,
            "sim": {
                "mains": self.sim["mains"],
                "tank_full": self.sim["tank_full"],
                "sump_dry": self.sim["sump_dry"],
                "current_override": self.sim["current_override"],
                "speed": self.sim["speed"],
                "now_epoch": now,
                "timezone": self.tz.tzname(None),
                "weekday": wd_names[dt.weekday()],
            },
        }

    def snapshot(self) -> dict:
        with self.lock:
            now = self.clock.now()
            self._advance_locked(now)
            return self._status_locked(now)

    def config_api(self) -> dict:
        with self.lock:
            return public_config(self.cfg)

    def events(self) -> list[dict]:
        with self.lock:
            now = self.clock.now()
            self._advance_locked(now)
            return [
                {
                    "t": int(e["t"]),
                    "time": datetime.fromtimestamp(e["t"], self.tz).strftime(
                        "%Y-%m-%d %H:%M:%S"
                    ),
                    "src": e["src"],
                    "msg": e["msg"],
                }
                for e in self.controller.events
            ]

    def set_config(self, incoming: dict) -> dict:
        with self.lock:
            now = self.clock.now()
            self._advance_locked(now)
            merged = merge_config(self.cfg, incoming)
            new_cfg = normalize_config(merged)
            self.cfg = new_cfg
            self.tz = parse_tz(new_cfg["device"]["timezone"])
            self.controller.tz = self.tz
            self.controller.apply_config(new_cfg)
            self._tick_once_locked(now)
            return public_config(self.cfg)

    def control(self, target: str, mode: str, relay_id: int = 0,
                boost_min: int = 0, pulse_ms: int = 0) -> dict:
        with self.lock:
            now = self.clock.now()
            self._advance_locked(now)
            if target == "relay":
                if not 1 <= relay_id <= MAX_CHANNELS:
                    raise ConfigError("relay id must be 1..12")
                self.controller.set_relay_mode(
                    relay_id - 1, mode, boost_min, now, pulse_ms
                )
            elif target == "pump":
                self.controller.set_pump_mode(mode, boost_min, now)
            elif target == "all":
                if mode == "off":
                    self.controller.all_off(now)
                elif mode == "auto":
                    self.controller.all_auto(now)
                else:
                    raise ConfigError("all supports auto/off only")
            else:
                raise ConfigError("target must be relay/pump/all")
            # keep the simulator config copy in sync with mode changes
            self.cfg = copy.deepcopy(self.controller.cfg)
            self._tick_once_locked(now)
            return self._status_locked(now)

    def reset_pump_fault(self) -> dict:
        with self.lock:
            now = self.clock.now()
            self._advance_locked(now)
            self.controller.reset_pump_fault(now)
            self._tick_once_locked(now)
            return self._status_locked(now)

    def set_time(self, epoch: float) -> dict:
        with self.lock:
            self.clock.set_epoch(epoch)
            self._sim_cursor = epoch
            self._last_tick_t = epoch
            # Drop any timers that would now be in the future after a jump.
            pr = self.controller.pump
            for attr in ("run_start", "last_off", "last_change"):
                if getattr(pr, attr) > epoch:
                    setattr(pr, attr, 0.0)
            for rt in self.controller.relays:
                if rt.last_change > epoch:
                    rt.last_change = 0.0
                if rt.boost_until > epoch:
                    rt.boost_until = 0.0
            if pr.boost_until > epoch:
                pr.boost_until = 0.0
            if self.controller.outage_start > epoch:
                self.controller.outage_start = 0.0
            if self.controller.restored_at > epoch:
                self.controller.restored_at = 0.0
            self._tick_once_locked(epoch)
            return self._status_locked(epoch)

    def sim_power(self, present: bool) -> dict:
        with self.lock:
            now = self.clock.now()
            self._advance_locked(now)
            self.sim["mains"] = bool(present)
            self._tick_once_locked(now)
            return self._status_locked(now)

    def sim_sensors(self, tank_full: Optional[bool] = None,
                    sump_dry: Optional[bool] = None) -> dict:
        with self.lock:
            now = self.clock.now()
            self._advance_locked(now)
            if tank_full is not None:
                self.sim["tank_full"] = bool(tank_full)
            if sump_dry is not None:
                self.sim["sump_dry"] = bool(sump_dry)
            self._tick_once_locked(now)
            return self._status_locked(now)

    def sim_current(self, auto: bool = False,
                    amps: Optional[float] = None) -> dict:
        with self.lock:
            now = self.clock.now()
            self._advance_locked(now)
            if auto:
                self.sim["current_override"] = None
            elif amps is not None:
                self.sim["current_override"] = max(0.0, float(amps))
            self._tick_once_locked(now)
            return self._status_locked(now)

    def sim_speed(self, speed: float) -> dict:
        with self.lock:
            now = self.clock.now()
            self._advance_locked(now)
            self.clock.set_speed(speed)
            self.sim["speed"] = max(0.01, float(speed))
            self._sim_cursor = self.clock.now()
            self._last_tick_t = self._sim_cursor
            return self._status_locked(self._sim_cursor)

    def sim_step(self, seconds: float) -> dict:
        with self.lock:
            self.clock.step(seconds)
            target = self.clock.now()
            self._advance_locked(target)
            return self._status_locked(target)

    def sim_reset(self) -> dict:
        with self.lock:
            self.controller = FarmController(self.cfg, self.tz)
            self.sim["mains"] = True
            self.sim["tank_full"] = False
            self.sim["sump_dry"] = False
            self.sim["current_override"] = None
            self.sim["battery_v"] = 13.2
            now = self.clock.now()
            self._sim_cursor = now
            self._last_tick_t = now
            self.controller.add_event("system", "simulator reset", now)
            self._tick_once_locked(now)
            return self._status_locked(now)


# ---------------------------------------------------------------------------
# HTTP server
# ---------------------------------------------------------------------------
class SimHTTPServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True
    app: SimApp


class SimHandler(BaseHTTPRequestHandler):
    server_version = "MRAFSim/1.0"
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt: str, *args: Any) -> None:
        if getattr(self.server, "app", None) and self.server.app.verbose:
            super().log_message(fmt, *args)

    # -- plumbing -----------------------------------------------------------
    def _send(self, code: int, body: Any,
              ctype: str = "application/json; charset=utf-8",
              extra: Optional[dict[str, str]] = None) -> None:
        if isinstance(body, (dict, list)):
            data = json.dumps(body).encode("utf-8")
        elif isinstance(body, str):
            data = body.encode("utf-8")
        else:
            data = bytes(body)
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        if extra:
            for k, v in extra.items():
                self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def _guard(self) -> bool:
        app: SimApp = self.server.app
        if self.path.split("?", 1)[0] == "/health":
            return True
        if not app.auth:
            return True
        header = self.headers.get("Authorization", "")
        if header.startswith("Basic "):
            try:
                user, password = base64.b64decode(header[6:]).decode("utf-8").split(":", 1)
            except Exception:
                user = password = ""
            if (user, password) == app.auth:
                return True
        self.close_connection = True
        self._send(401, {"ok": False, "error": "authentication required"},
                   extra={"WWW-Authenticate": 'Basic realm="MRAF"'})
        return False

    def _read_json(self) -> Optional[dict]:
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            length = 0
        raw = self.rfile.read(length) if length else b""
        if not raw:
            return {}
        try:
            data = json.loads(raw.decode("utf-8"))
        except Exception:
            self._send(400, {"ok": False, "error": "invalid JSON body"})
            return None
        if not isinstance(data, dict):
            self._send(400, {"ok": False, "error": "JSON body must be an object"})
            return None
        return data

    # -- routes -------------------------------------------------------------
    def do_GET(self) -> None:
        if not self._guard():
            return
        app: SimApp = self.server.app
        path = self.path.split("?", 1)[0]
        if path == "/":
            self._send(200, app.index_html, "text/html; charset=utf-8")
        elif path == "/health":
            self._send(200, {"ok": True, "device": "mraf", "simulator": True})
        elif path == "/api/status":
            self._send(200, app.snapshot())
        elif path == "/api/config":
            self._send(200, app.config_api())
        elif path == "/api/events":
            self._send(200, {"events": app.events()})
        elif path == "/api/wifi/scan":
            self._send(200, {"networks": []})
        else:
            self._send(404, {"ok": False, "error": "not found"})

    def do_POST(self) -> None:
        if not self._guard():
            return
        app: SimApp = self.server.app
        path = self.path.split("?", 1)[0]
        body = self._read_json()
        if body is None:
            return
        try:
            if path == "/api/config":
                self._send(200, {"ok": True, **app.set_config(body)})
            elif path == "/api/control":
                status = app.control(
                    str(body.get("target", "")),
                    str(body.get("mode", "auto")),
                    int(body.get("id") or 0),
                    int(body.get("boost_min") or 0),
                    int(body.get("pulse_ms") or 0),
                )
                self._send(200, {"ok": True, **status})
            elif path == "/api/time":
                epoch = body.get("epoch")
                if epoch is None:
                    raise ConfigError("epoch is required")
                self._send(200, {"ok": True, **app.set_time(float(epoch))})
            elif path == "/api/pump/reset":
                self._send(200, {"ok": True, **app.reset_pump_fault()})
            elif path == "/api/reboot":
                self._send(200, {"ok": True, "reboot": True})
            elif path == "/api/sim/power":
                self._send(200, {"ok": True, **app.sim_power(bool(body.get("present", True)))})
            elif path == "/api/sim/sensors":
                self._send(200, {"ok": True, **app.sim_sensors(
                    body.get("tank_full"), body.get("sump_dry"))})
            elif path == "/api/sim/current":
                self._send(200, {"ok": True, **app.sim_current(
                    bool(body.get("auto", False)),
                    body.get("amps"),
                )})
            elif path == "/api/sim/speed":
                self._send(200, {"ok": True, **app.sim_speed(float(body.get("speed", 60)))})
            elif path == "/api/sim/step":
                self._send(200, {"ok": True, **app.sim_step(float(body.get("seconds", 60)))})
            elif path == "/api/sim/reset":
                self._send(200, {"ok": True, **app.sim_reset()})
            else:
                self._send(404, {"ok": False, "error": "not found"})
        except (ConfigError, ValueError, TypeError) as exc:
            self._send(400, {"ok": False, "error": str(exc)})


# ---------------------------------------------------------------------------
# Scenarios / CLI
# ---------------------------------------------------------------------------
SCENARIOS = {
    "pump-morning":   (5, 59, 30, 60),
    "relay-evening":  (17, 59, 30, 60),
    "outage":         (6, 10, 0, 60),
    "dry-run":        (5, 59, 50, 60),
    "tank-full":      (5, 59, 50, 60),
    "sump-dry":       (5, 59, 50, 60),
    "night":          (21, 59, 30, 60),
}


def apply_scenario(app: SimApp, name: str) -> None:
    if name not in SCENARIOS:
        raise SystemExit(
            f"unknown scenario '{name}'. Choose from: {', '.join(sorted(SCENARIOS))}"
        )
    hour, minute, second, speed = SCENARIOS[name]
    now = datetime.now(app.tz).replace(
        hour=hour, minute=minute, second=second, microsecond=0
    )
    app.set_time(now.timestamp())
    app.sim_sensors(tank_full=(name == "tank-full"), sump_dry=(name == "sump-dry"))
    app.sim_power(True)
    if name == "dry-run":
        app.sim_current(auto=False, amps=0.0)
    else:
        app.sim_current(auto=True)
    if name == "outage":
        # Put the pump in manual run so the outage demo is visible immediately.
        app.control("pump", "on")
    app.sim_speed(speed)
    app.controller.add_event("sim", f"scenario {name}", app.clock.now())


def parse_epoch(value: str) -> float:
    try:
        return float(value)
    except ValueError:
        pass
    try:
        dt = datetime.fromisoformat(value)
    except ValueError as exc:
        raise SystemExit(f"invalid --epoch '{value}'") from exc
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return dt.timestamp()


def build_app(args: argparse.Namespace) -> SimApp:
    cfg = load_config(Path(args.config))
    if args.speed is not None and args.scenario is None:
        cfg.setdefault("sim", {})["speed"] = args.speed
    auth = None
    if args.auth:
        if ":" not in args.auth:
            raise SystemExit("--auth must be user:password")
        user, password = args.auth.split(":", 1)
        auth = (user, password)
    app = SimApp(cfg, speed=args.speed or 60.0, auth=auth,
                 host=args.host, port=args.port, verbose=args.verbose)
    if args.scenario:
        apply_scenario(app, args.scenario)
    if args.epoch:
        app.set_time(parse_epoch(args.epoch))
    return app


def main(argv: Optional[list[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        description="MRAF native farm-controller simulator (stdlib only)"
    )
    ap.add_argument("--host", default="0.0.0.0", help="bind address (default 0.0.0.0)")
    ap.add_argument("--port", type=int, default=8080, help="HTTP port (default 8080)")
    ap.add_argument("--config", default=str(DEFAULT_CONFIG), help="config JSON path")
    ap.add_argument("--speed", type=float, default=None,
                    help="simulated seconds per real second (default 60)")
    ap.add_argument("--epoch", default=None, help="start time: unix epoch or ISO-8601")
    ap.add_argument("--scenario", default=None,
                    help="pump-morning | relay-evening | outage | dry-run | tank-full | sump-dry | night")
    ap.add_argument("--auth", default=None, help="optional user:password for HTTP Basic auth")
    ap.add_argument("--verbose", action="store_true", help="log HTTP requests")
    args = ap.parse_args(argv)

    app = build_app(args)
    server = SimHTTPServer((args.host, args.port), SimHandler)
    server.app = app
    app.start()
    url = f"http://{'localhost' if args.host in ('0.0.0.0', '::') else args.host}:{args.port}/"
    print("MRAF native simulator")
    print(f"  dashboard : {url}")
    print(f"  timezone  : {app.tz.tzname(None)}")
    print(f"  speed     : {app.sim['speed']}x simulated seconds per real second")
    print(f"  scenario  : {args.scenario or 'none (live clock)'}")
    print("  press Ctrl+C to stop")
    try:
        server.serve_forever(poll_interval=0.25)
    except KeyboardInterrupt:
        print("\nstopping...")
    finally:
        app.stop()
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
