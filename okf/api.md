---
okf: 1
title: MRAF REST API
type: api
updated: 2026-11-08
tags: [rest, http, auth, json, api]
---

# REST API

Base URL: `http://mraf.local`, `http://<esp32-ip>` or the native build on
`http://127.0.0.1:8080`. The default credentials are `admin` / `farm1234`.

`GET /health` is public. Every other endpoint requires HTTP Basic auth when
credentials are configured. Errors are JSON:

```json
{"ok": false, "error": "authentication required"}
```

| Status | Meaning |
|---|---|
| 200 | Success |
| 400 | Invalid JSON, validation failure or unsupported control target |
| 401 | Missing or wrong Basic credentials |
| 404 | Unknown route |
| 405 | Wrong method for the route |
| 413 | Request body larger than the configured buffer |

## Endpoints

| Method | Path | Auth | Purpose |
|---|---|---|---|
| GET | `/health` | no | Liveness and firmware version |
| GET | `/` | yes | Dashboard HTML |
| GET | `/api/status` | yes | Live state and recent events |
| GET | `/api/config` | yes | Configuration without secrets |
| POST | `/api/config` | yes | Validate and persist configuration |
| POST | `/api/control` | yes | Relay, pump or all-load control |
| POST | `/api/pump/reset` | yes | Clear a latched pump fault |
| POST | `/api/time` | yes | Set system time and RTC |
| GET | `/api/events` | yes | Event log |
| GET | `/api/wifi/scan` | yes | Wi-Fi scan result |
| POST | `/api/reboot` | yes | Reboot the controller |

Simulator-only endpoints exist only when `CONFIG_MRAF_SIMULATOR=y`:
`/api/sim/power`, `/api/sim/sensors`, `/api/sim/current`, `/api/sim/speed`,
`/api/sim/step` and `/api/sim/reset`.

## Health

```bash
curl http://127.0.0.1:8080/health
```

```json
{"ok":true,"device":"mraf","fw":"1.1.0","simulator":true}
```

## Status

```bash
curl -u admin:farm1234 http://127.0.0.1:8080/api/status
```

The response contains:

- `now`, `now_local`, `time_valid`, `uptime_sec`, `fw`, `device`, `simulator`
- `wifi`: mode, ssid, ip, rssi, hostname, connected
- `remote`: CoAP enabled/host/port/interval, last poll result, pending request,
  registration status and device id
- `power`: mains present, battery voltage, outage seconds, battery-backed flag
- `sensors`: tank-full, sump-dry and ACS712 zero point
- `relays`: twelve objects with `id`, `name`, `zone`, `equipment`, `mode`,
  `on`, `output`, `pulsing`, `schedule`, `recurring`, `boost_until`,
  `last_change` and `windows`
- `pump`: name, mode, `on`, `want`, schedule/boost state, state name, fault
  flags, current, runtime and protection settings
- `events`: the most recent event records
- `sim`: simulator inputs when running on native_sim

## Control

```bash
curl -u admin:farm1234 -H 'Content-Type: application/json' \
  -d '{"target":"relay","id":1,"mode":"on","boost_min":30}' \
  http://127.0.0.1:8080/api/control
```

Accepted targets:

| Target | Body |
|---|---|
| `relay` | `id` 1-12, `mode` auto/on/off, optional `boost_min`, optional `pulse_ms` |
| `pump` | `mode` auto/on/off, optional `boost_min` |
| `all` | `mode` auto or off |

A successful response is the full status document plus `"ok": true`.

## Time

```bash
curl -u admin:farm1234 -H 'Content-Type: application/json' \
  -d '{"epoch":1800000000}' http://127.0.0.1:8080/api/time
```

Epoch must be at least 1700000000. On ESP32 the value is also written to the
DS3231. The response is the status document plus `"ok": true`.

## Configuration

`GET /api/config` returns the configuration without `auth_pass`,
`wifi.password`, `coap.token` or `coap.pass`. `POST /api/config` accepts the
same shape, validates it, persists it and returns it with:

```json
{"ok":true,"reboot_required":false}
```

Missing fields keep their current values. Changing Wi-Fi SSID/password,
hostname, auth user or auth password sets `reboot_required` to true.

The JSON schema is in [../shared/config.schema.json](../shared/config.schema.json).
The top-level objects are `device`, `wifi`, `coap`, `channels`, `pump` and
`safety`.

## Events

```bash
curl -u admin:farm1234 http://127.0.0.1:8080/api/events
```

```json
{"events":[{"t":1800000000,"time":"2027-01-15 12:00:00","src":"system","msg":"boot"}]}
```

## Simulator controls

```bash
curl -u admin:farm1234 -H 'Content-Type: application/json' \
  -d '{"present":false}' http://127.0.0.1:8080/api/sim/power
curl -u admin:farm1234 -H 'Content-Type: application/json' \
  -d '{"tank_full":true,"sump_dry":false}' http://127.0.0.1:8080/api/sim/sensors
curl -u admin:farm1234 -H 'Content-Type: application/json' \
  -d '{"amps":2.5}' http://127.0.0.1:8080/api/sim/current
curl -u admin:farm1234 -H 'Content-Type: application/json' \
  -d '{"speed":60}' http://127.0.0.1:8080/api/sim/speed
curl -u admin:farm1234 -H 'Content-Type: application/json' \
  -d '{"seconds":60}' http://127.0.0.1:8080/api/sim/step
curl -u admin:farm1234 -X POST http://127.0.0.1:8080/api/sim/reset
```

Each simulator control returns the resulting status document with
`"ok": true`.
