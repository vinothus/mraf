import json
import os
import time
import urllib.error
import urllib.request
from base64 import b64encode

BASE = os.environ.get("MRAF_BASE_URL", "http://192.168.0.201")
AUTH_USER = os.environ.get("MRAF_AUTH_USER")
AUTH_PASS = os.environ.get("MRAF_AUTH_PASS")
if not AUTH_USER or not AUTH_PASS:
    raise SystemExit("set MRAF_AUTH_USER and MRAF_AUTH_PASS before running this test")
AUTH = "Basic " + b64encode(f"{AUTH_USER}:{AUTH_PASS}".encode()).decode()
RELAY_ID = 2
ON_PIN = int(os.environ.get("MRAF_ON_PIN", "14"))
OFF_PIN = int(os.environ.get("MRAF_OFF_PIN", "15"))
PULSE_ON_MS = int(os.environ.get("MRAF_PULSE_ON_MS", os.environ.get("MRAF_PULSE_MS", "1500")))
PULSE_OFF_MS = int(os.environ.get("MRAF_PULSE_OFF_MS", os.environ.get("MRAF_PULSE_MS", "1500")))
CYCLES = 2
rows = []


def record(name, ok, detail):
    rows.append((name, "PASS" if ok else "FAIL", detail))


def call(path, method="GET", payload=None):
    data = None if payload is None else json.dumps(payload).encode()
    req = urllib.request.Request(BASE + path, data=data, method=method)
    req.add_header("Authorization", AUTH)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(req, timeout=30) as response:
        return json.load(response)


def relay(status):
    return next(r for r in status["relays"] if r["id"] == RELAY_ID)


def status():
    return call("/api/status")


def control(mode):
    return call("/api/control", "POST", {
        "target": "relay",
        "id": RELAY_ID,
        "mode": mode,
        "pulse_ms": PULSE_ON_MS if mode == "on" else PULSE_OFF_MS,
    })


def wait_release(expected_on, seconds=8):
    deadline = time.time() + seconds
    sample = relay(status())
    while time.time() < deadline:
        sample = relay(status())
        if not sample["output"] and not sample["pulsing"] and sample["on"] == expected_on:
            break
        time.sleep(0.5)
    return sample


original = None
baseline_pin = None
baseline_off_pin = None
try:
    original = call("/api/config")
    original_channel = original["channels"][RELAY_ID - 1]
    baseline_pin = original_channel.get("pin", original["pins"]["relays"][RELAY_ID - 1])
    baseline_off_pin = original_channel.get("pin_off", original["pins"]["relays_off"][RELAY_ID - 1])
    record("Baseline config loaded", bool(original_channel),
           f"pin={baseline_pin}, pin_off={baseline_off_pin}")

    test_channel = dict(original_channel)
    test_channel.update({
        "pin": ON_PIN,
        "pin_off": OFF_PIN,
        "equipment": "motor",
        "pulse_mode": True,
        "pulse_on_ms": PULSE_ON_MS,
        "pulse_off_ms": PULSE_OFF_MS,
        "enabled": True,
        "mode": "off",
    })
    apply_result = call("/api/config", "POST", {
        "safety": {"assume_mains_present": True},
        "channels": [{}, test_channel],
    })
    record("Apply motor pulse config", apply_result.get("ok") is True,
           str(apply_result)[:160])
    time.sleep(1)
    mapped = relay(status())
    record("Separate ON/OFF GPIO mapping",
           mapped.get("pin") == ON_PIN and mapped.get("pin_off") == OFF_PIN and ON_PIN != OFF_PIN,
           f"on={mapped.get('pin')}, off={mapped.get('pin_off')}, expected={ON_PIN}/{OFF_PIN}")

    control("off")
    time.sleep(0.3)

    for cycle in range(1, CYCLES + 1):
        sample = relay(control("on"))
        record(f"ON pulse {cycle} energized on GPIO {ON_PIN}",
               sample["output"] and sample["pulsing"] and sample["on"] and
               sample.get("output_pin") == ON_PIN,
               f"output={sample['output']}, pulsing={sample['pulsing']}, on={sample['on']}, "
               f"active_gpio={sample.get('output_pin')}")
        time.sleep(PULSE_ON_MS / 1000.0 + 0.6)
        sample = wait_release(True)
        record(f"ON pulse {cycle} released",
               not sample["output"] and not sample["pulsing"] and sample["on"],
               f"output={sample['output']}, pulsing={sample['pulsing']}, on={sample['on']}")

        sample = relay(control("off"))
        record(f"OFF pulse {cycle} energized on GPIO {OFF_PIN}",
               sample["output"] and sample["pulsing"] and not sample["on"] and
               sample.get("output_pin") == OFF_PIN,
               f"output={sample['output']}, pulsing={sample['pulsing']}, on={sample['on']}, "
               f"active_gpio={sample.get('output_pin')}")
        time.sleep(PULSE_OFF_MS / 1000.0 + 0.6)
        sample = wait_release(False)
        record(f"OFF pulse {cycle} released",
               not sample["output"] and not sample["pulsing"] and not sample["on"],
               f"output={sample['output']}, pulsing={sample['pulsing']}, on={sample['on']}")

    events = status()["events"][-12:]
    control_events = [e for e in events if e["src"] == "control"]
    record("Control events logged", len(control_events) >= CYCLES * 2,
           f"{len(control_events)} control events")
except urllib.error.HTTPError as exc:
    record("HTTP error", False, f"HTTP {exc.code}: {exc.read().decode()[:200]}")
except Exception as exc:
    record("Exception", False, repr(exc))
finally:
    if original is not None:
        try:
            restore_channel = dict(original["channels"][RELAY_ID - 1])
            if baseline_pin is not None:
                restore_channel["pin"] = baseline_pin
            if baseline_off_pin is not None:
                restore_channel["pin_off"] = baseline_off_pin
            call("/api/config", "POST", {
                "safety": {"assume_mains_present": original["safety"]["assume_mains_present"]},
                "channels": [{}, restore_channel],
            })
        except Exception:
            pass
    try:
        call("/api/control", "POST", {"target": "relay", "id": RELAY_ID, "mode": "auto"})
    except Exception:
        pass

for name, state, detail in rows:
    print(f"{state} | {name} | {detail}")
print(f"TOTAL {sum(1 for _, s, _ in rows if s == 'PASS')}/{len(rows)} PASS")
