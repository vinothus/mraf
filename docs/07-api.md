# 07 - REST API

Base URL: `http://mraf.local` or `http://<esp32-ip>`. The simulator uses the
same paths on `http://localhost:8080`.

## Authentication

`GET /health` is public. All other endpoints use HTTP Basic authentication when
an auth user/password is configured (default `admin` / `farm1234`).

```bash
curl -u admin:farm1234 http://mraf.local/api/status
```

Errors are JSON:

```json
{"ok": false, "error": "authentication required"}
```

Status codes: `200` success, `400` bad request/validation, `401` auth required,
`404` unknown route.

## Endpoints

| Method | Path | Purpose |
|---|---|---|
| GET | `/health` | Liveness and firmware version (no auth) |
| GET | `/` | Dashboard HTML |
| GET | `/api/status` | Live relay, pump, power, sensor, WiFi and event data |
| GET | `/api/config` | Full configuration without secrets |
| POST | `/api/config` | Validate and save configuration |
| POST | `/api/control` | Relay, pump or all-load control |
| POST | `/api/pump/reset` | Clear a latched pump fault |
| POST | `/api/time` | Set the system time and RTC |
| GET | `/api/events` | Event log |
| GET | `/api/wireguard` | WireGuard overlay state |
| POST | `/api/wireguard` | Apply a server-style WireGuard config |
| POST | `/api/chat` | Deterministic grammar chat |
| GET | `/api/webrtc` | Peer-to-peer link settings for the browser (see [13-webrtc.md](13-webrtc.md)) |
| POST | `/mcp` | MCP JSON-RPC 2.0 endpoint |
| GET | `/api/wifi/scan` | Scan Wi-Fi networks |
| POST | `/api/reboot` | Reboot the ESP32 |

Simulator-only:

| Method | Path | Purpose |
|---|---|---|
| POST | `/api/sim/power` | Set mains present |
| POST | `/api/sim/sensors` | Set tank-full / sump-dry |
| POST | `/api/sim/current` | Auto or fixed pump current |
| POST | `/api/sim/speed` | Simulated seconds per real second |
| POST | `/api/sim/step` | Advance the simulated clock |
| POST | `/api/sim/reset` | Reset simulator state |

## GET /health

```bash
curl http://mraf.local/health
```

```json
{"ok": true, "device": "mraf", "fw": "1.0.0"}
```

The simulator adds `"simulator": true`.

## GET /api/status

```json
{
  "simulator": false,
  "fw": "1.0.0",
  "device": "Neer Kaavalan",
  "now": 1793579400,
  "now_local": "2026-11-02 06:00:00",
  "time_valid": true,
  "uptime_sec": 3600,
  "wifi": {
    "mode": "sta",
    "ssid": "farm-wifi",
    "ip": "192.168.1.50",
    "rssi": -61,
    "hostname": "mraf",
    "connected": true
  },
  "remote": {
    "enabled": true,
    "host": "192.168.1.20",
    "port": 5683,
    "path": "/mraf",
    "interval_sec": 30,
    "last_code": 68,
    "last_ok": true,
    "last_error": "",
    "last_poll": 1793579400,
    "pending": false,
    "status": "confirmed",
    "confirmed": true,
    "device_id": "ESP32-A1B2C3",
    "user": "farm",
    "wireguard": {
      "enabled": true,
      "configured": true,
      "active": true,
      "interface": "wg0",
      "address": "10.66.66.2/32",
      "endpoint": "vpn.example.net:51820"
    }
  },
  "profile": "agri",
  "power": {
    "present": true,
    "battery_v": 13.21,
    "outage_sec": 0,
    "battery_backed": true
  },
  "sensors": {
    "tank_full": false,
    "sump_dry": false,
    "acs_zero_mv": 1250.0
  },
  "relays": [
    {
      "id": 1,
      "name": "Shed Light",
      "zone": "Shed",
      "equipment": "light",
      "pulse_mode": false,
      "pulse_on_ms": 800,
      "pulse_off_ms": 600,
      "enabled": true,
      "days": 127,
      "mode": "auto",
      "on": false,
      "schedule": false,
      "boost_until": 0,
      "last_change": 1793570000,
      "windows": [{"on": "18:00", "off": "22:00"}]
    }
  ],
  "pump": {
    "name": "5HP Borewell Pump",
    "enabled": true,
    "days": 127,
    "mode": "auto",
    "on": true,
    "want": true,
    "schedule": true,
    "boost_until": 0,
    "state": "RUNNING",
    "fault": "",
    "fault_latched": false,
    "current_a": 6.82,
    "run_sec": 12,
    "last_off": 1793577300,
    "last_change": 1793579390,
    "external_run": false,
    "tank_full": false,
    "sump_dry": false,
    "min_off_sec": 300,
    "max_run_min": 120,
    "recovery_delay_sec": 30,
    "resume_after_outage": true,
    "dry_run_current_a": 1.5,
    "dry_run_grace_sec": 20,
    "windows": [
      {"on": "06:00", "off": "07:00"},
      {"on": "17:00", "off": "18:00"}
    ]
  },
  "events": [
    {"t": 1793579390, "time": "2026-11-02 05:59:50", "src": "pump", "msg": "started"}
  ]
}
```

`relays` contains 12 entries. The example is truncated.

## GET /api/config

Returns the configuration in the same shape accepted by `POST /api/config`.
Secrets (`device.auth_pass`, `wifi.password`, `coap.token`) are omitted.

The JSON schema is [`shared/config.schema.json`](../shared/config.schema.json).

## POST /api/config

Send a full or partial configuration. Missing fields keep their current values.
Empty `auth_pass`, Wi-Fi `password` or `coap.token` also keeps the existing
secret.

```bash
curl -u admin:farm1234 \
  -H 'Content-Type: application/json' \
  -X POST http://mraf.local/api/config \
  -d '{
    "device": {
      "name": "North Farm",
      "timezone": "IST-5:30",
      "profile": "agri"
    },
    "wifi": {"ssid": "farm-wifi", "password": "secret"},
    "coap": {
      "enabled": true,
      "host": "192.168.1.20",
      "port": 5683,
      "path": "/mraf",
      "interval_sec": 30,
      "token": "shared-secret",
      "user": "farm",
      "pass": "farm-pass",
      "register": true
    },
    "channels": [
      {"id": 1, "name": "Shed Light", "zone": "Shed", "equipment": "light",
       "pulse_mode": false, "pulse_on_ms": 800, "pulse_off_ms": 600,
       "enabled": true, "days": 127, "mode": "auto",
       "windows": [{"on": "18:00", "off": "22:00"}]},
      {"id": 5, "name": "Borewell Starter", "zone": "Well",
       "equipment": "motor", "pulse_mode": true,
       "pulse_on_ms": 800, "pulse_off_ms": 600,
       "enabled": true, "days": 127, "mode": "auto",
       "windows": [{"on": "06:00", "off": "07:00"}]}
    ],
    "recurring": [
      {"name": "Weekly soak", "repeat": "weekly", "days": 1,
       "from": "05:00", "to": "05:15", "channel_mask": 5, "action": "on"}
    ],
    "pump": {
      "enabled": true,
      "days": 127,
      "mode": "auto",
      "windows": [{"on": "06:00", "off": "07:00"}],
      "min_off_sec": 300,
      "max_run_min": 120,
      "recovery_delay_sec": 30,
      "resume_after_outage": true,
      "dry_run_current_a": 1.5,
      "dry_run_grace_sec": 20
    }
  }'
```

Response:

```json
{"ok": true, "reboot_required": false}
```

`reboot_required` is true when Wi-Fi, hostname or auth settings changed; the
controller responds first, then reboots. The saved configuration is fetched
back with `GET /api/config`; the POST reply is intentionally kept small so the
device does not build a second full config document while the first is still
in memory.

Validation rules include:

- `relay_count` selects how many relays are active (1-12). `channels` is
  trimmed/padded to that length; the dashboard and `/api/status` expose only
  the active relays. Omitted `relay_count` defaults to the number of supplied
  channels (or 12 for older configs).
- `device.profile` is `agri`, `home`, `godown`, `industry` or a custom lowercase
  slug from the Profiles tab (`a-z`, `0-9`, `-`, `_`, max 23 chars).
- `equipment` is `valve`, `light`, `motor`, `fan`, `pump`, `socket`, `heater`,
  `door` or `other`.
- `pulse_on_ms` and `pulse_off_ms` are 20-10000 ms. `pulse_mode` enables
  momentary operation for motors and contactors.
- `days` is 0-127 (bit 0 = Monday).
- `mode` is `auto`, `on` or `off`.
- Times are `HH:MM`, 00:00-23:59.
- Up to 4 windows per channel/pump.
- `pins.relays` maps relay 1-12 to ESP32 GPIOs, `pins.relays_off` optionally
  maps a second STOP/contactor GPIO for each relay, `pins.pump` maps the pump
  starter START pulse GPIO, and `pins.pump_off` maps its STOP pulse GPIO. A DOL
  pump starter is **always** pulse-driven: the ON window pulses `pins.pump` for
  `pump.pulse_on_ms` and the OFF window pulses `pins.pump_off` for
  `pump.pulse_off_ms`; both coils then release. Set `pins.pump` to `0` to delete
  the starter from a profile (dryland / manavari farming with no pump); the
  dashboard then hides the pump card and the controller ignores the pump
  schedule. When a motor relay uses START/STOP push buttons, the ON window
  pulses `relays[i]` for `pulse_on_ms` and the OFF window pulses
  `relays_off[i]` for `pulse_off_ms`; both coils then release. When
  `relays_off[i]` equals `relays[i]`, the single-GPIO latching relay behavior
  is unchanged. Only safe output GPIOs are accepted (2, 4, 5, 12-19, 21-23,
  25-27, 32, 33) and no pin may be shared. `channels[].pin`/`pin_off` are
  accepted as aliases for `pins.relays[i]`/`pins.relays_off[i]`.
- `profiles` is the optional profile library edited by the Profiles tab. Each
  profile has an `id`, `relay_count`, `name`, `unit`, `pins` and that many
  `channels` with a single
  `on`/`off` window plus `pulse_on_ms`/`pulse_off_ms`. The firmware applies the
  selected profile's pins/channels on "Apply to device"; the simulator stores
  the library in its config file, while hardware keeps only the active profile
  and the browser caches the library locally. Use **Pull from device** in the
  Profiles tab to discard a stale browser cache and reload the active profile
  from `/api/config` (or `/api/profiles` on the simulator).
- Up to 8 recurring schedules. `repeat` is `daily`, `weekly`, `monthly` or
  `yearly`; `day_of_month` 1-31, `month` 1-12, `action` is `on`, `off` or
  `auto`, and channels are selected with `channel_mask` (bit 0 = relay 1) or a
  `channels` id array. Off rules win over on rules.
- `POST /api/selftest` briefly pulses relay coils and the pump starter START and
  STOP coils and returns `checks[]` with PASS/WARN/FAIL per ESP32 subsystem,
  relay coil, contactor and sensor, plus an overall `summary`. A profile with no
  pump starter reports it as not fitted instead of pulsing a GPIO.
- CoAP `port` 1-65535, `path` starts with `/`, `interval_sec` 10-3600,
  `user`/`pass` are the server login stored at registration.
- `min_off_sec` 30-3600, `max_run_min` 1-720, `recovery_delay_sec` 0-600,
  `dry_run_current_a` 0.5-20, `dry_run_grace_sec` 5-120, `max_boost_min`
  1-1440. `safety.assume_mains_present` defaults to `false`; set it to `true`
  for USB-powered installations without a mains-sense input so schedules and
  manual relay/pump commands are not blocked by the outage gate.

## POST /api/control

Relay:

```bash
curl -u admin:farm1234 -H 'Content-Type: application/json' \
  -X POST http://mraf.local/api/control \
  -d '{"target":"relay","id":3,"mode":"on"}'
```

Momentary pulse (motor starter or contactor):

```bash
curl -u admin:farm1234 -H 'Content-Type: application/json' \
  -X POST http://mraf.local/api/control \
  -d '{"target":"relay","id":5,"mode":"on","pulse_ms":800}'
```

Pump:

```bash
curl -u admin:farm1234 -H 'Content-Type: application/json' \
  -X POST http://mraf.local/api/control \
  -d '{"target":"pump","mode":"auto"}'
```

All loads:

```bash
curl -u admin:farm1234 -H 'Content-Type: application/json' \
  -X POST http://mraf.local/api/control \
  -d '{"target":"all","mode":"off"}'
```

Fields:

| Field | Values |
|---|---|
| `target` | `relay`, `pump`, `all` |
| `id` | 1-12 for relay targets |
| `mode` | `auto`, `on`, `off` (`all` supports only `auto`/`off`) |
| `boost_min` | 0-1440; capped by `max_boost_min`; forces Auto mode |
| `pulse_ms` | 0-10000; momentary ON/OFF duration for a relay; `0` uses the channel pulse timing |

The response is the full status object plus `"ok": true`.

## POST /api/pump/reset

Clears `fault_latched` and the fault message. It does not bypass tank/sump,
minimum-off or recovery timers.

```bash
curl -u admin:farm1234 -X POST http://mraf.local/api/pump/reset
```

## POST /api/time

```bash
curl -u admin:farm1234 -H 'Content-Type: application/json' \
  -X POST http://mraf.local/api/time \
  -d '{"epoch":1793579400}'
```

Epoch must be at least 1700000000. The RTC is updated when present, and the
epoch is saved to persistent storage for boot after power loss. The response is
the full status object plus `"ok": true`.

In the dashboard, **Settings → Time & maintenance** accepts device-local wall
time. The browser converts it to epoch using the configured POSIX timezone, so
with `IST-5:30` (UTC+05:30), entering `10:30 AM` sends `05:00 UTC`.

## GET /api/events

Returns up to 48 stored events; the status object includes the last 30.

## GET /api/wifi/scan

Returns visible networks:

```json
{"networks": [{"ssid": "farm-wifi", "rssi": -61, "secure": true,
               "channel": 6, "band": "2.4 GHz"}],
 "radio": "2.4 GHz", "count": 1, "scanned": 1}
```

Duplicate SSIDs heard on multiple APs are collapsed to the strongest signal.
`error` is set (instead of `networks` being silently empty) when the radio
could not run a scan, e.g. while it is busy connecting. The ESP32 radio scans
the 2.4 GHz band only; 5 GHz-only SSIDs never appear.

Scanning temporarily interrupts the station connection and blocks an HTTP
worker for up to 8 s. Use it from the Settings page; the dashboard pauses its
status polling while a scan is in flight.

## POST /api/reboot

```bash
curl -u admin:farm1234 -X POST http://mraf.local/api/reboot
```

Returns `{"ok": true, "reboot": true}` then restarts.

## GET /api/wireguard

Returns the overlay state:

```json
{"enabled":true,"configured":true,"active":true,"interface":"wg0",
 "address":"10.66.66.2/32","endpoint":"vpn.example.net:51820",
 "allowed_ips":"10.66.66.0/24","persistent_keepalive":25}
```

The same object is embedded in `/api/status` under `remote.wireguard`.

## POST /api/wireguard

Accepts the flat payload the server sends in a CoAP `wireguard` request and
applies it. See [10-remote-coap.md](10-remote-coap.md) for the field list.

## POST /api/chat

Runs the on-device deterministic grammar (no language model):

```bash
curl -u admin:farm1234 -H 'Content-Type: application/json' \
  -d '{"text":"turn on relay 3"}' http://mraf.local/api/chat
```

Actions are `help`, `status`, `data`, `control` and `unknown`. The `status`
action embeds the full status document. The server mirrors the same grammar in
`com.mraf.server.intent.IntentParser`.

## GET /api/webrtc

Returns the peer-to-peer link settings the browser needs to open a WebRTC data
channel to this device directly:

```bash
curl -u admin:farm1234 http://mraf.local/api/webrtc
```

```json
{
  "enabled": true,
  "server": "http://10.0.0.5:8085",
  "stun": "stun:10.0.0.5:3478",
  "poll_ms": 400,
  "device_id": "ESP32-A1B2C3",
  "token": "demo-token",
  "transport": "datachannel"
}
```

The `token` is the shared signalling token; the device exposes it here (over its
own authenticated HTTP) so the browser can talk to the Spring Boot control
plane. See [13-webrtc.md](13-webrtc.md).

## POST /mcp

MCP JSON-RPC 2.0 (Streamable HTTP, JSON response mode). Methods: `initialize`,
`notifications/*` (HTTP 202), `ping`, `tools/list` and `tools/call`. Tools:
`get_status`, `control_relay`, `control_pump`, `set_all`, `apply_config`,
`wireguard_status` and `chat`. Requires `CONFIG_MRAF_MCP=y` (default).

## Simulator-only endpoints

These are intentionally absent from the firmware.

```bash
curl -H 'Content-Type: application/json' -X POST http://localhost:8080/api/sim/power \
  -d '{"present": false}'

curl -H 'Content-Type: application/json' -X POST http://localhost:8080/api/sim/sensors \
  -d '{"tank_full": true, "sump_dry": false}'

curl -H 'Content-Type: application/json' -X POST http://localhost:8080/api/sim/current \
  -d '{"auto": false, "amps": 0}'

curl -H 'Content-Type: application/json' -X POST http://localhost:8080/api/sim/speed \
  -d '{"speed": 300}'

curl -H 'Content-Type: application/json' -X POST http://localhost:8080/api/sim/step \
  -d '{"seconds": 120}'

curl -X POST http://localhost:8080/api/sim/reset
```

`/api/sim/step` advances the simulated clock immediately, which is useful for
automated tests. `/api/sim/speed` changes the real-time acceleration factor.
