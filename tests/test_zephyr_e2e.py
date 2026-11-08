#!/usr/bin/env python3
"""End-to-end tests for the Zephyr firmware running on native_sim."""
import base64
import json
import os
import subprocess
import tempfile
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "firmware/zephyr/build/native/zephyr/zephyr.exe"
PORT = int(os.environ.get("MRAF_E2E_PORT", "18080"))
BASE = f"http://127.0.0.1:{PORT}"
AUTH = "Basic " + base64.b64encode(b"admin:farm1234").decode()


class Server:
    def __init__(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="mraf-e2e-")
        env = os.environ.copy()
        env["MRAF_HTTP_PORT"] = str(PORT)
        self.log = open(Path(self.tmp.name) / "firmware.log", "wb")
        self.proc = subprocess.Popen(
            [str(BINARY)],
            cwd=self.tmp.name,
            env=env,
            stdout=self.log,
            stderr=subprocess.STDOUT,
        )

    def wait_ready(self, timeout=20.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("firmware exited before becoming ready")
            try:
                with urllib.request.urlopen(BASE + "/health", timeout=1) as resp:
                    if resp.status == 200:
                        return
            except (urllib.error.URLError, ConnectionError, TimeoutError):
                pass
            time.sleep(0.2)
        raise TimeoutError(f"firmware did not listen on {BASE}")

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        self.log.close()
        log_path = Path(self.tmp.name) / "firmware.log"
        if log_path.exists():
            tail = log_path.read_text(errors="replace")[-4000:]
            if tail.strip():
                print("\n--- firmware log tail ---\n" + tail)
        self.tmp.cleanup()


def request(path, method="GET", body=None, auth=True):
    data = None
    if body is not None:
        data = json.dumps(body).encode()
    req = urllib.request.Request(BASE + path, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    if auth:
        req.add_header("Authorization", AUTH)
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return resp.status, resp.read(), resp.headers
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read(), exc.headers


def request_json(path, method="GET", body=None, auth=True):
    status, payload, _ = request(path, method, body, auth)
    try:
        return status, json.loads(payload)
    except json.JSONDecodeError:
        return status, payload


class ZephyrE2ETests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not BINARY.exists():
            raise unittest.SkipTest(f"missing {BINARY}; run make zephyr-build-native")
        cls.server = Server()
        cls.server.wait_ready()

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()

    def test_01_health_is_public(self):
        status, doc = request_json("/health", auth=False)
        self.assertEqual(status, 200)
        self.assertTrue(doc["ok"])
        self.assertEqual(doc["device"], "mraf")
        self.assertEqual(doc["fw"], "1.1.0")
        self.assertTrue(doc["simulator"])

    def test_02_status_requires_auth(self):
        status, _, _ = request("/api/status", auth=False)
        self.assertEqual(status, 401)
        status, doc = request_json("/api/status")
        self.assertEqual(status, 200)
        self.assertIn("relays", doc)
        self.assertIn("pump", doc)
        self.assertIn("wifi", doc)
        self.assertEqual(len(doc["relays"]), 12)

    def test_03_dashboard_served(self):
        status, payload, headers = request("/")
        self.assertEqual(status, 200)
        self.assertIn("text/html", headers.get("Content-Type", ""))
        self.assertIn(b"<title>MRAF Controller", payload)

    def test_04_config_roundtrip(self):
        status, config = request_json("/api/config")
        self.assertEqual(status, 200)
        status, doc = request_json("/api/config", method="POST", body=config)
        self.assertEqual(status, 200)
        self.assertTrue(doc["ok"])
        self.assertIn("reboot_required", doc)
        status, again = request_json("/api/config")
        self.assertEqual(status, 200)
        self.assertEqual(again["device"]["name"], config["device"]["name"])
        self.assertEqual(again["device"]["timezone"], config["device"]["timezone"])
        self.assertEqual(len(again["channels"]), len(config["channels"]))

    def test_05_relay_control(self):
        status, doc = request_json(
            "/api/control", method="POST",
            body={"target": "relay", "id": 1, "mode": "on"},
        )
        self.assertEqual(status, 200)
        self.assertEqual(doc["relays"][0]["mode"], "on")
        self.assertTrue(doc["relays"][0]["on"])

        status, doc = request_json(
            "/api/control", method="POST",
            body={"target": "relay", "id": 1, "mode": "off"},
        )
        self.assertEqual(status, 200)
        self.assertEqual(doc["relays"][0]["mode"], "off")
        self.assertFalse(doc["relays"][0]["on"])

        status, doc = request_json(
            "/api/control", method="POST", body={"target": "all", "mode": "auto"},
        )
        self.assertEqual(status, 200)
        self.assertEqual(doc["relays"][0]["mode"], "auto")

    def test_06_pump_control_and_reset(self):
        status, doc = request_json(
            "/api/control", method="POST",
            body={"target": "pump", "mode": "off"},
        )
        self.assertEqual(status, 200)
        self.assertEqual(doc["pump"]["mode"], "off")
        self.assertFalse(doc["pump"]["on"])

        status, doc = request_json("/api/pump/reset", method="POST")
        self.assertEqual(status, 200)
        self.assertTrue(doc["ok"])
        self.assertFalse(doc["pump"]["fault"])

        status, doc = request_json(
            "/api/control", method="POST", body={"target": "pump", "mode": "auto"},
        )
        self.assertEqual(status, 200)
        self.assertEqual(doc["pump"]["mode"], "auto")

    def test_07_set_time(self):
        epoch = 1800000000
        status, doc = request_json(
            "/api/time", method="POST", body={"epoch": epoch},
        )
        self.assertEqual(status, 200)
        self.assertTrue(doc["ok"])
        self.assertGreaterEqual(doc["now"], epoch)
        self.assertTrue(doc["time_valid"])

        status, doc = request_json(
            "/api/time", method="POST", body={"epoch": 100},
        )
        self.assertEqual(status, 400)

    def test_08_sim_power_and_sensors(self):
        status, doc = request_json(
            "/api/sim/power", method="POST", body={"present": False},
        )
        self.assertEqual(status, 200)
        self.assertFalse(doc["power"]["present"])
        self.assertFalse(doc["sim"]["mains"])

        status, doc = request_json(
            "/api/sim/power", method="POST", body={"present": True},
        )
        self.assertEqual(status, 200)
        self.assertTrue(doc["power"]["present"])
        self.assertTrue(doc["sim"]["mains"])

        status, doc = request_json(
            "/api/sim/sensors", method="POST",
            body={"tank_full": True, "sump_dry": True},
        )
        self.assertEqual(status, 200)
        self.assertTrue(doc["sensors"]["tank_full"])
        self.assertTrue(doc["sensors"]["sump_dry"])

    def test_09_sim_current_speed_and_step(self):
        status, doc = request_json(
            "/api/sim/current", method="POST", body={"amps": 2.5},
        )
        self.assertEqual(status, 200)
        self.assertAlmostEqual(doc["sim"]["current_override"], 2.5, places=2)

        status, doc = request_json(
            "/api/sim/speed", method="POST", body={"speed": 60},
        )
        self.assertEqual(status, 200)
        before = doc["now"]
        status, doc = request_json(
            "/api/sim/step", method="POST", body={"seconds": 60},
        )
        self.assertEqual(status, 200)
        self.assertGreaterEqual(doc["now"], before + 60)

        status, doc = request_json("/api/sim/reset", method="POST")
        self.assertEqual(status, 200)
        self.assertTrue(doc["ok"])

    def test_10_events_and_scan(self):
        status, doc = request_json("/api/events")
        self.assertEqual(status, 200)
        self.assertIsInstance(doc["events"], list)
        self.assertGreater(len(doc["events"]), 0)
        self.assertIn("msg", doc["events"][0])

        status, doc = request_json("/api/wifi/scan")
        self.assertEqual(status, 200)
        self.assertIn("networks", doc)

    def test_11_invalid_control(self):
        status, doc = request_json(
            "/api/control", method="POST",
            body={"target": "relay", "id": 99, "mode": "on"},
        )
        self.assertEqual(status, 400)
        self.assertIn("error", doc)


if __name__ == "__main__":
    unittest.main(verbosity=2)
