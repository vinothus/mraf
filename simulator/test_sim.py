#!/usr/bin/env python3
"""Native tests for the MRAF simulator/controller. Stdlib only."""
import copy
import json
import threading
import unittest
import urllib.error
import urllib.request
from datetime import datetime
from pathlib import Path

from mraf_sim import (
    ConfigError,
    FarmController,
    SimApp,
    SimHTTPServer,
    SimHandler,
    load_config,
    normalize_config,
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
