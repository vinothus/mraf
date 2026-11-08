# 04 - Firmware architecture

## Files

| File | Purpose |
|---|---|
| `firmware/mraf/mraf.ino` | Setup/loop, Wi-Fi, HTTP routes, sensor sampling, JSON status |
| `firmware/mraf/controller.h/.cpp` | Relay and pump state machine, timers, events (also compiled by the native host test) |
| `firmware/mraf/schedule.h` | Pure scheduling logic (also compiled by the native C++ test) |
| `firmware/mraf/settings.h/.cpp` | Defaults, NVS persistence, JSON import/export/validation |
| `firmware/mraf/rtc.h/.cpp` | Optional DS3231 driver on a dedicated I2C bus |
| `firmware/mraf/coap_client.h/.cpp` | Minimal CoAP confirmable POST client over UDP |
| `firmware/mraf/config.h` | Pin map, analog scaling, compile-time defaults |
| `firmware/mraf/model.h` | `Config`, runtime structs, enums |
| `firmware/mraf/webui.h` | Generated from `web/index.html` by `tools/gen_webui.py` |

There is no dynamic allocation in the controller hot path. Configuration is a
fixed-size struct. JSON documents are allocated by ArduinoJson only while
serving a request.

## Main loop

1. `server.handleClient()` serves the web UI and API.
2. Mains presence is polled every 50 ms with an 800 ms debounce.
3. Sensors, current, battery and time are sampled once per second.
4. `Controller::tick()` evaluates schedules, modes and the pump state machine.
5. The CoAP poll runs when enabled and the Wi-Fi station link is up.
6. The status LED is updated and events are logged.

The controller uses wall-clock epoch seconds, not `millis()`, so the same logic
works in the accelerated simulator and after an NTP/RTC time jump.

## Scheduling

Each channel has:

- `enabled`
- `days` (bit 0 = Monday ... bit 6 = Sunday)
- up to 4 windows (`on`/`off` in local HH:MM)
- `mode` (`auto`, `on`, `off`)
- optional boost until a timestamp

`scheduleActive()` handles same-day and cross-midnight windows. For a window
that crosses midnight, the evening part belongs to the current day's bit and
the morning part belongs to the previous day's bit. For example, a Monday
18:30-05:30 window is active on Monday evening and Tuesday morning, but not
Monday morning.

A window with `on == off` is treated as never active to avoid ambiguity.

## Pump state machine

The pump has its own dedicated relay output (GPIO33) and these runtime fields:

- `want`: the current logical request
- `on`: the contactor command
- `schedule`: schedule result for this minute
- `fault_latched`: a fault that requires a manual reset
- `external_run`: current detected while the firmware is not commanding the pump
- `need_edge`: used when resume-after-outage is disabled
- `run_start`, `last_off`, `last_change`
- filtered `current_a`

### Start conditions

The pump starts only when all are true:

1. Mains is present.
2. Pump is enabled and no fault is latched.
3. Tank is not full and sump is not dry.
4. Mode is `on`, or mode is `auto` and the schedule or boost requests it.
5. Minimum off time has elapsed.
6. Recovery delay after power return has elapsed.
7. If `resume_after_outage` is false, a new schedule edge has occurred.

### Stop conditions

The pump stops immediately when:

- mains is lost;
- mode becomes `off`;
- the schedule/boost request ends;
- tank becomes full or sump becomes dry;
- a fault is latched.

Stopping records `last_off`, which feeds the minimum-off timer.

### Faults

| Fault | Trigger | Reset |
|---|---|---|
| Dry run / no current | Run time exceeds grace and filtered current < `dry_run_current_a` | Manual `POST /api/pump/reset` |
| Maximum run time | Continuous run exceeds `max_run_min` | Manual reset |
| Tank full | Sense input active | Clears when the tank is no longer full; pump restarts only if the schedule still wants it |
| Sump dry | Sense input active | Clears when the sump is no longer dry |
| Power loss | Mains sense lost | Automatic after recovery delay, subject to resume policy |

A latched fault never auto-restarts. The dashboard shows `FAULT` and the event
log records the reason.

### External run detection

If the controller sees pump current while it is not commanding the pump, it
reports `EXTERNAL` (someone used the HAND starter). It will not fight the
external run. When the current disappears, it records the stop time so the
minimum-off timer still applies.

## Power-loss behaviour

- The relay coils are on the mains-derived 5 V SMPS, so every relay drops out
  when mains is lost.
- The ESP32, RTC and sensors stay on the battery-backed 5 V buck.
- The controller logs `mains lost`, records the outage start, and marks
  `need_edge` if resume is disabled.
- On restore, it logs `mains restored`, records `restored_at`, and waits for
  `recovery_delay_sec` before starting the pump.
- If `resume_after_outage` is true and the schedule still wants the pump, it
  restarts after the recovery delay and minimum-off time.
- If false, it waits for the schedule to go inactive and then active again
  (a new edge) before starting.

## Remote CoAP

When `coap.enabled` is set, the controller POSTs JSON to the configured host,
port and path at the configured interval. The response can be:

- a control request, which is applied using the same rules as
  `POST /api/control`;
- a data request, which makes the controller send a compact status snapshot on
  the next poll;
- a configuration request, which is validated by the same code as
  `POST /api/config`, saved to NVS and acknowledged on the next poll;
- `{"request":"none"}`.

Control, data and configuration results are acknowledged on the next poll with
the request `id`. Duplicate control or config `id` values are not re-applied.
The firmware uses a 4096-byte request buffer and an 8192-byte response buffer;
the Java server raises Californium's datagram limits to 8192 so a full
12-channel configuration with recurring schedules fits one datagram. The client
is a small CoAP confirmable POST implementation on `WiFiUDP`; it does not use
DTLS. See
[10-remote-coap.md](10-remote-coap.md) for the wire contract and
[11-springboot-server.md](11-springboot-server.md) for the included server and
dashboard.

## Configuration and NVS

Configuration is stored in the ESP32 `Preferences` (NVS) namespace `mraf`.
On boot:

1. Load factory defaults.
2. Load NVS if present.
3. If NVS is invalid, keep defaults.
4. Apply timezone and start the RTC/NTP.

`POST /api/config` validates the JSON before saving. Missing fields keep their
current values. Empty `auth_pass` and Wi-Fi `password` keep the existing secret.
Changing Wi-Fi, hostname, auth user or auth password triggers a reboot after
the response is sent.

The API never returns the auth password or Wi-Fi password.

## Time

- Timezone is a POSIX TZ string, default `IST-5:30` (UTC+05:30).
- NTP servers: `pool.ntp.org`, `time.google.com`, `in.pool.ntp.org`.
- DS3231 is read at boot and written after a manual time set.
- The dashboard and API display local time.
- `POST /api/time` accepts a Unix epoch and updates the system clock and RTC.

## Web UI and API

`web/index.html` is a single-page dashboard. It is embedded into the firmware
as `webui.h` at build time. The same file is served by the native simulator.

The UI provides:

- live relay and pump state;
- per-channel Auto/On/Off and boost;
- schedule editor with day masks and multiple windows;
- pump settings, safety thresholds and boost limit;
- Wi-Fi scan and credentials;
- event log;
- simulator-only controls (mains, floats, current, speed, time step).

HTTP Basic authentication is enforced when an auth user/password is configured.
The default is `admin` / `farm1234`; change it before connecting the controller
to a shared network.

## Native tests

`make test-cpp` builds and runs two host tests with g++:

- `tests/test_schedule.cpp` covers the pure schedule helpers: day masks,
  same-day windows, cross-midnight windows, previous-day rules and zero-length
  windows.
- `tests/test_controller.cpp` compiles the real `controller.cpp` against
  `tests/shim/Arduino.h` and checks schedule start/stop, float interlocks,
  dry-run and max-run trips, fault reset, minimum off time, outage recovery and
  resume-edge behavior, and relay manual/boost/all-off control.

The controller test is why `controller.cpp` does not depend on ArduinoJson or
NVS: only `controller.h`, `model.h`, `config.h` and `schedule.h` are needed.
`make test` runs both host tests and the Python simulator suite.

## Memory and build

Verified with Arduino-ESP32 3.3.12 and ArduinoJson 7.4.3:

```text
Sketch uses 1,121,494 bytes (85%) of program storage space.
Global variables use 58,356 bytes (17%) of dynamic memory.
```

The default app partition has 1,310,720 bytes. There is enough headroom for
the current UI and libraries, but not for OTA. If you add many features, select
a larger partition scheme or trim the web UI. There is no OTA update path in
this version.

## Constants worth knowing

| Constant | Default | Meaning |
|---|---|---|
| `DEFAULT_MIN_OFF_SEC` | 300 | 5 min anti-short-cycle |
| `DEFAULT_MAX_RUN_MIN` | 120 | 2 h continuous limit |
| `DEFAULT_RECOVERY_DELAY_SEC` | 30 | wait after mains return |
| `DEFAULT_DRY_RUN_CURRENT_A` | 1.5 | below this while running = dry |
| `DEFAULT_DRY_RUN_GRACE_SEC` | 20 | ignore current during start |
| `DEFAULT_MAX_BOOST_MIN` | 180 | maximum manual boost |
| `EVENT_LOG_SIZE` | 48 | in-RAM event ring |
| `MAX_CHANNELS` | 12 | scheduled relay count |
| `MAX_WINDOWS` | 4 | windows per channel/pump |
