# 08 - Native simulator

`simulator/mraf_sim.py` is a standard-library-only digital twin of the
controller. It runs on Linux (and any Python 3.10+ OS) without an ESP32, without
Wokwi, and without internet.

It shares the same dashboard, HTTP API shape and pump/schedule rules as the
firmware. It is deliberately a separate implementation: the Python controller
mirrors the C++ state machine, so the simulator is useful for training and for
catching logic mistakes before flashing hardware.

## Run

```bash
cd /workspace/mraf
make sim
# http://localhost:8080
```

or directly:

```bash
python3 simulator/mraf_sim.py --speed 60 --port 8080
```

Options:

| Option | Default | Meaning |
|---|---|---|
| `--host` | `0.0.0.0` | Bind address |
| `--port` | `8080` | HTTP port |
| `--config` | `simulator/config.json` | Starting configuration |
| `--speed` | `60` | Simulated seconds per real second |
| `--epoch` | now | Start time as Unix epoch or ISO-8601 |
| `--scenario` | none | Preset scenario |
| `--auth` | none | Optional `user:password` for HTTP Basic auth |
| `--verbose` | off | Log HTTP requests |

Examples:

```bash
python3 simulator/mraf_sim.py --speed 300 --port 8080
python3 simulator/mraf_sim.py --epoch 2026-11-02T05:59:30 --speed 60
python3 simulator/mraf_sim.py --auth admin:farm1234
```

The simulator writes nothing to disk. Stop it with Ctrl+C.

## Scenarios

| Scenario | Start time | What it demonstrates |
|---|---|---|
| `pump-morning` | 05:59:30 | Pump starts at 06:00, current appears |
| `relay-evening` | 17:59:30 | Channel 1 starts at 18:00, channel 2 at 18:30 |
| `outage` | 06:10:00 | Pump in manual run; toggle mains to see outage/recovery |
| `dry-run` | 05:59:50 | Pump starts with zero current and trips after grace |
| `tank-full` | 05:59:50 | Tank-full float blocks the pump |
| `sump-dry` | 05:59:50 | Sump-dry float blocks the pump |
| `night` | 21:59:30 | Aerators start at 22:00 |

Run:

```bash
make sim-scenario SCENARIO=pump-morning
make sim-scenario SCENARIO=outage
```

The accelerated clock means a 60-second schedule window takes about one real
second at `--speed 60`.

## Dashboard simulator panel

The shared dashboard shows a **Simulator** card when `status.simulator` is true:

- Mains present toggle.
- Tank full / sump dry toggles.
- Pump current: auto or fixed amps.
- Speed selector (1x, 10x, 60x, 300x, 1800x).
- Step +1 min / +10 min.
- Reset simulator.

These controls call the `/api/sim/*` endpoints. The firmware does not expose
them.

## Clock model

`SimClock` stores a base epoch and a monotonic base time. The current simulated
time is:

```text
now = base_epoch + (monotonic_now - base_mono) * speed
```

`sim_step` changes the base epoch immediately. `set_time` jumps the clock and
resets any pump/relay timers that would be in the future after the jump, so an
API time change cannot leave the controller stuck behind a future `last_off`.

At high speeds the simulator advances in 1-second or 5-second steps. A 0.2 s
real-time loop at 60x therefore processes about 12 simulated seconds per
iteration.

## Controller parity

The Python controller implements the same rules as `controller.cpp`:

- schedule windows and cross-midnight day masks;
- relay Auto/On/Off and boost;
- pump min-off, max-run, dry-run, tank/sump interlocks;
- mains loss, recovery delay, resume policy and `need_edge`;
- external run detection;
- latched faults and manual reset;
- event ring buffer.

The simulator's battery model is intentionally simple: the 12.8 V LiFePO4
pack charges toward 13.6 V when mains is present and drains slowly during an
outage. It is only used for a low-battery event and dashboard display.

## Tests

```bash
make test-sim
# or
cd simulator && python3 -m unittest -v test_sim
```

The suite covers:

- same-day and cross-midnight schedules;
- day masks, including the previous-day rule for the morning part;
- scheduled pump start and running current;
- tank-full and sump-dry blocking;
- dry-run and max-run trips;
- fault reset;
- minimum-off restart;
- outage recovery with and without resume;
- relay schedules, cross-midnight relay, manual and boost;
- config validation;
- HTTP health, auth, control, config and simulator endpoints.

Tests construct `SimApp` with speed 0 and set an explicit epoch, so they do not
depend on the real time of day.

## Writing a scenario test

```python
from datetime import datetime
from simulator.mraf_sim import SimApp, load_config

cfg = load_config("simulator/config.json")
app = SimApp(cfg, speed=0)
app.set_time(datetime(2026, 11, 2, 5, 59, 30, tzinfo=app.tz).timestamp())
app.sim_step(60)
assert app.controller.pump.on
assert app.controller.pump.current_a > 6
```

Use `sim_power()`, `sim_sensors()` and `sim_current()` to inject faults. Use
`app.snapshot()` for the same status JSON returned by `/api/status`.

## Differences from the firmware

- No real ADC, relay, I2C or Wi-Fi hardware. Sensors are explicit inputs.
- Wi-Fi mode is reported as connected simulation; there is no AP fallback.
- The simulator has no NVS. Configuration lives in memory and `config.json`.
- The simulator adds `/api/sim/*`.
- CoAP settings are stored and shown, but the simulator does not poll a remote
  server.
- The simulator's `/health` is public, matching the firmware.
- Time is accelerated and can jump with `/api/time` or `/api/sim/step`.
- The simulator does not model contactor bounce, motor thermal mass or
  electrical noise.

## Wokwi

`hardware/wokwi/` contains a small browser wiring check for the real firmware.
It is useful for LED/sensor pin verification, not for schedule or API testing.
See [`hardware/wokwi/README.md`](../hardware/wokwi/README.md).
