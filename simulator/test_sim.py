#!/usr/bin/env python3
"""Native tests for the MRAF simulator/controller. Stdlib only."""
import copy
import json
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

from mraf_sim import (
    ConfigError,
    FarmController,
    SimApp,
    SimHTTPServer,
    SimHandler,
    load_config,
    merge_config,
    normalize_config,
    parse_intent,
)

CONFIG = Path(__file__).resolve().parent / "config.json"


def epoch(app, year, month, day, hour, minute, second=0):
    return datetime(year, month, day, hour, minute, second, tzinfo=app.tz).timestamp()


def make_app(at=(2026, 11, 2, 5, 59, 30), **pump_overrides):
    cfg = load_config(CONFIG)
    cfg["pump"].update(pump_overrides)
    app = SimApp(cfg, speed=0)
    app.set_time(epoch(app, *at))
    return app


class ScheduleTests(unittest.TestCase):
    def test_same_day_window(self):
        w = [(360, 390)]
        self.assertTrue(FarmController._schedule_active(w, 127, 0, 360))
        self.assertTrue(FarmController._schedule_active(w, 127, 0, 389))
        self.assertFalse(FarmController._schedule_active(w, 127, 0, 390))
        self.assertFalse(FarmController._schedule_active(w, 127, 0, 359))

    def test_cross_midnight_uses_previous_day(self):
        w = [(1110, 330)]  # 18:30 -> 05:30
        # Monday evening, Tuesday morning, Tuesday evening.
        self.assertTrue(FarmController._schedule_active(w, 1, 0, 1200))
        self.assertTrue(FarmController._schedule_active(w, 1, 1, 300))
        self.assertFalse(FarmController._schedule_active(w, 1, 2, 300))
        self.assertFalse(FarmController._schedule_active(w, 1, 1, 600))

    def test_zero_length_window_never_active(self):
        w = [(360, 360)]
        self.assertFalse(FarmController._schedule_active(w, 127, 0, 360))


class PumpTests(unittest.TestCase):
    def test_scheduled_start(self):
        app = make_app()
        app.sim_step(60)
        self.assertTrue(app.controller.pump.on)
        self.assertEqual(app.controller.pump_state_name(app.clock.now()), "RUNNING")

    def test_tank_full_blocks_start(self):
        app = make_app()
        app.sim_sensors(tank_full=True)
        app.sim_step(120)
        self.assertFalse(app.controller.pump.on)
        self.assertTrue(app.controller.pump.tank_full)

    def test_sump_dry_blocks_start(self):
        app = make_app()
        app.sim_sensors(sump_dry=True)
        app.sim_step(120)
        self.assertFalse(app.controller.pump.on)
        self.assertTrue(app.controller.pump.sump_dry)

    def test_dry_run_trips(self):
        app = make_app()
        app.sim_current(auto=False, amps=0.0)
        app.sim_step(120)
        self.assertTrue(app.controller.pump.fault_latched)
        self.assertIn("dry run", app.controller.pump.fault)
        self.assertFalse(app.controller.pump.on)

    def test_max_run_trips(self):
        app = make_app(max_run_min=1)
        app.sim_step(120)
        self.assertTrue(app.controller.pump.fault_latched)
        self.assertIn("max run", app.controller.pump.fault)

    def test_outage_recovery_with_resume(self):
        app = make_app(min_off_sec=30)
        app.sim_step(60)
        self.assertTrue(app.controller.pump.on)
        app.sim_power(False)
        self.assertFalse(app.controller.pump.on)
        app.sim_step(10)
        app.sim_power(True)
        app.sim_step(10)
        self.assertFalse(app.controller.pump.on)  # recovery delay not elapsed
        app.sim_step(30)
        self.assertTrue(app.controller.pump.on)

    def test_outage_without_resume_waits_for_edge(self):
        app = make_app(min_off_sec=30, resume_after_outage=False)
        app.sim_step(60)
        self.assertTrue(app.controller.pump.on)
        app.sim_power(False)
        app.sim_power(True)
        app.sim_step(120)
        self.assertFalse(app.controller.pump.on)
        self.assertTrue(app.controller.pump.need_edge)

    def test_min_off_blocks_restart(self):
        app = make_app(min_off_sec=300)
        app.sim_step(60)
        self.assertTrue(app.controller.pump.on)
        app.control("pump", "off")
        app.control("pump", "auto")
        app.sim_step(120)
        self.assertFalse(app.controller.pump.on)
        app.sim_step(240)
        self.assertTrue(app.controller.pump.on)

    def test_fault_reset_allows_restart(self):
        app = make_app(max_run_min=1, min_off_sec=30)
        app.sim_step(120)
        self.assertTrue(app.controller.pump.fault_latched)
        app.reset_pump_fault()
        app.sim_step(40)
        self.assertFalse(app.controller.pump.fault_latched)
        self.assertTrue(app.controller.pump.on)


class RelayTests(unittest.TestCase):
    def test_evening_relay(self):
        app = make_app(at=(2026, 11, 2, 17, 59, 30))
        app.sim_step(60)
        self.assertTrue(app.controller.relays[0].on)
        self.assertFalse(app.controller.relays[1].on)

    def test_cross_midnight_relay(self):
        app = make_app(at=(2026, 11, 2, 23, 0, 0))
        app.sim_step(2)
        self.assertTrue(app.controller.relays[1].on)  # Yard Light
        app.set_time(epoch(app, 2026, 11, 3, 5, 0, 0))
        app.sim_step(2)
        self.assertTrue(app.controller.relays[1].on)  # Tuesday follows Monday bit
        app.set_time(epoch(app, 2026, 11, 3, 6, 0, 0))
        app.sim_step(2)
        self.assertFalse(app.controller.relays[1].on)

    def test_manual_override_and_boost(self):
        app = make_app()
        app.control("relay", "on", 1)
        app.sim_step(2)
        self.assertTrue(app.controller.relays[0].on)
        app.control("relay", "off", 1)
        app.sim_step(2)
        self.assertFalse(app.controller.relays[0].on)
        app.control("relay", "auto", 1, boost_min=1)
        app.sim_step(2)
        self.assertTrue(app.controller.relays[0].on)
        app.sim_step(61)
        self.assertFalse(app.controller.relays[0].on)


class ConfigTests(unittest.TestCase):
    def test_invalid_window_rejected(self):
        cfg = load_config(CONFIG)
        bad = copy.deepcopy(cfg)
        bad["channels"][0]["windows"] = [{"on": "25:00", "off": "06:00"}]
        with self.assertRaises(ConfigError):
            normalize_config(bad)

    def test_too_many_channels_rejected(self):
        cfg = load_config(CONFIG)
        bad = copy.deepcopy(cfg)
        bad["channels"] = (bad["channels"] * 2)[:13]
        with self.assertRaises(ConfigError):
            normalize_config(bad)

    def test_public_config_removes_sim(self):
        app = make_app()
        cfg = app.config_api()
        self.assertNotIn("sim", cfg)
        self.assertEqual(len(cfg["channels"]), 12)

    def test_public_config_removes_coap_token(self):
        app = make_app()
        app.set_config({"coap": {"token": "shared-secret"}})
        self.assertNotIn("token", app.config_api()["coap"])

    def test_coap_token_survives_public_roundtrip(self):
        app = make_app()
        app.set_config({"coap": {"token": "shared-secret"}})
        app.set_config(app.config_api())
        self.assertEqual(app.cfg["coap"]["token"], "shared-secret")

    def test_coap_validation(self):
        cfg = load_config(CONFIG)
        bad = copy.deepcopy(cfg)
        bad["coap"]["enabled"] = True
        bad["coap"]["host"] = ""
        with self.assertRaises(ConfigError):
            normalize_config(bad)
        bad = copy.deepcopy(cfg)
        bad["coap"]["path"] = "mraf"
        with self.assertRaises(ConfigError):
            normalize_config(bad)
        bad = copy.deepcopy(cfg)
        bad["coap"]["port"] = 70000
        with self.assertRaises(ConfigError):
            normalize_config(bad)
        bad = copy.deepcopy(cfg)
        bad["coap"]["interval_sec"] = 5
        with self.assertRaises(ConfigError):
            normalize_config(bad)

    def test_public_config_removes_wireguard_secrets(self):
        app = make_app()
        app.wireguard_configure({
            "interface": "wg0",
            "address": "10.66.66.2/32",
            "private_key": "priv-secret",
            "peer": {"public_key": "pub", "endpoint": "vpn:51820",
                     "allowed_ips": "10.66.66.0/24", "preshared_key": "psk"},
        })
        wg = app.config_api()["wireguard"]
        self.assertNotIn("private_key", wg)
        self.assertNotIn("peer_preshared_key", wg)
        self.assertTrue(wg["configured"])


class WebRtcConfigTests(unittest.TestCase):
    def test_defaults_present_and_disabled(self):
        cfg = normalize_config(load_config(CONFIG))
        wrc = cfg["webrtc"]
        self.assertFalse(wrc["enabled"])
        self.assertEqual(wrc["server"], "")
        self.assertEqual(wrc["poll_ms"], 400)

    def test_enable_roundtrip(self):
        app = make_app()
        app.set_config({"webrtc": {"enabled": True, "server": "http://server:8085"}})
        self.assertTrue(app.cfg["webrtc"]["enabled"])
        self.assertEqual(app.cfg["webrtc"]["server"], "http://server:8085")

    def test_config_api_exposes_peer_params(self):
        app = make_app()
        app.set_config({"webrtc": {"enabled": True, "server": "http://server:8085"},
                        "coap": {"token": "shared-secret"}})
        info = app.webrtc_config()
        self.assertTrue(info["enabled"])
        self.assertEqual(info["server"], "http://server:8085")
        self.assertEqual(info["token"], "shared-secret")
        self.assertEqual(info["transport"], "datachannel")

    def test_public_config_does_not_leak_token(self):
        app = make_app()
        app.set_config({"webrtc": {"enabled": True, "server": "http://server:8085"},
                        "coap": {"token": "shared-secret"}})
        wrc = app.config_api()["webrtc"]
        self.assertNotIn("token", wrc)
        self.assertTrue(wrc["enabled"])

    def test_status_reports_peer_state(self):
        app = make_app()
        status = app.webrtc_status()
        for key in ("enabled", "available", "state", "connected", "rx_bytes", "tx_bytes"):
            self.assertIn(key, status)


class IntentTests(unittest.TestCase):
    def test_relay_on(self):
        intent = parse_intent("turn on relay 3")
        self.assertEqual((intent["action"], intent["target"], intent["relay_id"],
                          intent["mode"]), ("control", "relay", 3, "on"))

    def test_pulse(self):
        intent = parse_intent("pulse relay 2 for 800 ms")
        self.assertEqual((intent["target"], intent["relay_id"], intent["pulse_ms"]),
                         ("relay", 2, 800))

    def test_boost_default(self):
        intent = parse_intent("boost the pump")
        self.assertEqual((intent["target"], intent["mode"], intent["boost_min"]),
                         ("pump", "auto", 30))

    def test_all_on_becomes_auto(self):
        intent = parse_intent("all on")
        self.assertEqual((intent["target"], intent["mode"]), ("all", "auto"))

    def test_status(self):
        self.assertEqual(parse_intent("status")["action"], "status")

    def test_unknown_relay(self):
        intent = parse_intent("relay 99 off")
        self.assertFalse(intent["understood"])
        self.assertEqual(intent["action"], "unknown")

    def test_help(self):
        self.assertEqual(parse_intent("help")["action"], "help")


class ApiTests(unittest.TestCase):
    def setUp(self):
        self.app = make_app()
        self.server = SimHTTPServer(("127.0.0.1", 0), SimHandler)
        self.server.app = self.app
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = f"http://127.0.0.1:{self.port}"

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.app.stop()

    def get(self, path, auth=None):
        req = urllib.request.Request(self.base + path)
        if auth:
            import base64
            token = base64.b64encode(auth.encode()).decode()
            req.add_header("Authorization", "Basic " + token)
        with urllib.request.urlopen(req, timeout=5) as r:
            return json.loads(r.read().decode())

    def post(self, path, body, auth=None):
        data = json.dumps(body).encode()
        req = urllib.request.Request(self.base + path, data=data,
                                     headers={"Content-Type": "application/json"})
        if auth:
            import base64
            token = base64.b64encode(auth.encode()).decode()
            req.add_header("Authorization", "Basic " + token)
        with urllib.request.urlopen(req, timeout=5) as r:
            return json.loads(r.read().decode())

    def test_health_and_status(self):
        self.assertEqual(self.get("/health")["device"], "mraf")
        status = self.get("/api/status")
        self.assertEqual(len(status["relays"]), 12)
        self.assertIn("pump", status)
        self.assertTrue(status["simulator"])

    def test_self_test_reports_hardware_checks(self):
        data = self.post("/api/selftest", {})
        self.assertTrue(data["ok"])
        self.assertIn("PASS", data["summary"])
        self.assertGreaterEqual(data["total"], 18)
        names = [c["name"] for c in data["checks"]]
        self.assertIn("ESP32 controller", names)
        self.assertIn("Relay 1 driver", names)
        self.assertIn("Pump contactor", names)
        relay = next(c for c in data["checks"] if c["name"] == "Relay 1 driver")
        self.assertIn("GPIO", relay["detail"])
        self.assertEqual(relay["status"], "pass")

    def test_auth_required_when_configured(self):
        self.app.auth = ("admin", "farm1234")
        with self.assertRaises(urllib.error.HTTPError) as ctx:
            self.get("/api/status")
        self.assertEqual(ctx.exception.code, 401)
        self.assertEqual(self.get("/health")["device"], "mraf")
        status = self.get("/api/status", "admin:farm1234")
        self.assertIn("pump", status)

    def test_control_and_config_roundtrip(self):
        r = self.post("/api/control", {"target": "relay", "id": 1, "mode": "on"})
        self.assertTrue(r["relays"][0]["on"])
        cfg = self.get("/api/config")
        cfg["pump"]["max_run_min"] = 77
        r = self.post("/api/config", cfg, "admin:farm1234" if self.app.auth else None)
        self.assertEqual(r["pump"]["max_run_min"], 77)
        self.assertEqual(self.get("/api/config")["pump"]["max_run_min"], 77)

    def test_sim_endpoints(self):
        r = self.post("/api/sim/power", {"present": False})
        self.assertFalse(r["sim"]["mains"])
        r = self.post("/api/sim/power", {"present": True})
        self.assertTrue(r["sim"]["mains"])
        r = self.post("/api/sim/step", {"seconds": 60})
        self.assertIn("now", r)

    def test_chat_endpoint(self):
        r = self.post("/api/chat", {"text": "turn on relay 3"})
        self.assertEqual(r["action"], "control")
        self.assertTrue(r["applied"])
        status = self.get("/api/status")
        self.assertTrue(status["relays"][2]["on"])

    def test_chat_status_embeds_snapshot(self):
        r = self.post("/api/chat", {"text": "status"})
        self.assertEqual(r["action"], "status")
        self.assertIn("relays", r["status"])

    def test_wireguard_endpoints(self):
        before = self.get("/api/wireguard")
        self.assertFalse(before["configured"])
        r = self.post("/api/wireguard", {
            "interface": "wg0", "address": "10.66.66.2/32", "private_key": "priv",
            "peer": {"public_key": "pub", "endpoint": "vpn:51820",
                     "allowed_ips": "10.66.66.0/24", "persistent_keepalive": 25},
        })
        self.assertEqual(r["result"], "applied")
        self.assertTrue(r["wireguard"]["active"])
        status = self.get("/api/status")
        self.assertTrue(status["remote"]["wireguard"]["active"])

    def test_mcp_tools_and_call(self):
        listing = self.post("/mcp", {"jsonrpc": "2.0", "id": 1, "method": "tools/list"})
        names = [t["name"] for t in listing["result"]["tools"]]
        self.assertIn("control_relay", names)
        self.assertIn("wireguard_status", names)
        call = self.post("/mcp", {"jsonrpc": "2.0", "id": 2, "method": "tools/call",
                                  "params": {"name": "control_relay",
                                             "arguments": {"relay_id": 2, "mode": "on"}}})
        self.assertFalse(call["result"]["isError"])

    def test_mcp_notification_returns_202(self):
        data = json.dumps({"jsonrpc": "2.0",
                           "method": "notifications/initialized"}).encode()
        req = urllib.request.Request(self.base + "/mcp", data=data,
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=5) as r:
            self.assertEqual(r.status, 202)
            self.assertEqual(r.read(), b"")


class ProfileTests(unittest.TestCase):
    def sample_profile(self):
        return {
            "id": "greenhouse", "name": "Greenhouse", "unit": "Bay",
            "pins": {"relays": [13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27],
                     "relays_off": [13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27],
                     "pump": 32},
            "channels": [
                {"id": 1, "name": "Misting", "equipment": "valve",
                 "pulse_mode": True, "pulse_on_ms": 800, "pulse_off_ms": 900,
                 "on": "05:30", "off": "05:45", "enabled": True, "days": 21}
            ],
        }

    def test_custom_profile_and_runtime_pins(self):
        cfg = load_config(CONFIG)
        cfg["device"]["profile"] = "greenhouse"
        cfg["profiles"] = [self.sample_profile()]
        cfg["pins"] = self.sample_profile()["pins"]
        out = normalize_config(cfg)
        self.assertEqual(out["device"]["profile"], "greenhouse")
        self.assertEqual(out["pins"]["pump"], 32)
        self.assertEqual(out["profiles"][0]["relay_count"], 1)
        self.assertEqual(out["profiles"][0]["channels"][0]["pulse_on_ms"], 800)
        self.assertEqual(len(out["profiles"][0]["channels"]), 1)

    def test_relay_count_limits_channels_and_status(self):
        cfg = normalize_config(load_config(CONFIG))
        cfg["relay_count"] = 2
        out = normalize_config(cfg)
        self.assertEqual(len(out["channels"]), 2)
        self.assertEqual(out["channels"][1]["id"], 2)
        app = SimApp(out, speed=0)
        self.assertEqual(len(app.snapshot()["relays"]), 2)
        with self.assertRaises(ConfigError):
            app.control("relay", "on", 3)

    def test_relay_count_merge_trims_channels(self):
        cfg = load_config(CONFIG)
        merged = merge_config(cfg, {"relay_count": 2, "channels": [{"id": 1}, {"id": 2}]})
        out = normalize_config(merged)
        self.assertEqual(out["relay_count"], 2)
        self.assertEqual(len(out["channels"]), 2)

    def test_duplicate_gpio_rejected(self):
        cfg = load_config(CONFIG)
        cfg["pins"] = {"relays": [13] * 12, "pump": 33}
        with self.assertRaises(ConfigError):
            normalize_config(cfg)

    def test_invalid_gpio_rejected(self):
        cfg = load_config(CONFIG)
        cfg["pins"]["pump"] = 34  # input-only on ESP32
        with self.assertRaises(ConfigError):
            normalize_config(cfg)

    def test_motor_contactor_pulses_start_then_stop_pin(self):
        cfg = normalize_config(load_config(CONFIG))
        cfg["pins"]["relays_off"][4] = 32
        ch = cfg["channels"][4]
        ch["pulse_mode"] = True
        ch["pulse_on_ms"] = 120
        ch["pulse_off_ms"] = 180
        ctrl = FarmController(cfg, timezone.utc)

        ctrl.set_relay_mode(4, "on", 0, 1000)
        rt = ctrl.relays[4]
        self.assertEqual(rt.pulse_pin, 18)
        self.assertAlmostEqual(rt.pulse_until, 1000.12)
        ctrl.service_pulses(1000.13)
        self.assertFalse(rt.output)

        ctrl.set_relay_mode(4, "off", 0, 2000)
        self.assertEqual(rt.pulse_pin, 32)
        self.assertAlmostEqual(rt.pulse_until, 2000.18)
        ctrl.service_pulses(2000.19)
        self.assertFalse(rt.output)

    def test_stop_pin_must_be_unique(self):
        cfg = load_config(CONFIG)
        cfg["pins"]["relays_off"][0] = cfg["pins"]["relays"][1]
        with self.assertRaises(ConfigError):
            normalize_config(cfg)

    def test_profile_config_is_persisted(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "config.json"
            path.write_text(json.dumps(load_config(CONFIG)), encoding="utf-8")
            app = SimApp(load_config(CONFIG), speed=0, config_path=path)
            profile = self.sample_profile()
            app.set_config({"device": {"profile": "greenhouse"},
                            "pins": profile["pins"],
                            "profiles": [profile],
                            "channels": [{"id": 1, "name": "Misting"}]})
            saved = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(saved["device"]["profile"], "greenhouse")
            self.assertEqual(saved["pins"]["pump"], 32)
            self.assertEqual(saved["profiles"][0]["id"], "greenhouse")
            self.assertEqual(app.config_api()["channels"][0]["name"], "Misting")


if __name__ == "__main__":
    unittest.main(verbosity=2)
