---
okf: 1
title: MRAF REST API
type: api
updated: 2026-11-14
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
| GET | `/api/wireguard` | yes | WireGuard overlay state |
| POST | `/api/wireguard` | yes | Apply a server-style WireGuard config |
| POST | `/api/chat` | yes | Deterministic grammar chat |
| GET | `/api/webrtc` | yes | Peer-to-peer link settings for the browser |
| POST | `/mcp` | yes | MCP JSON-RPC 2.0 endpoint |
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
  registration status, device id and a `wireguard` object (`enabled`,
  `configured`, `active`, `interface`, `address`, `endpoint`, `allowed_ips`,
  `persistent_keepalive`). The controller also answers server `console`
  requests by running the command on its on-device Zephyr shell and returning
  the captured output in the ACK (see
  [../docs/10-remote-coap.md](../docs/10-remote-coap.md)).
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
The top-level objects are `device`, `wifi`, `coap`, `wireguard`, `channels`,
`pump` and `safety`.

## WireGuard

`GET /api/wireguard` returns the overlay state. `POST /api/wireguard` accepts the
flat payload the server sends in a CoAP `wireguard` request and applies it:

```bash
curl -u admin:farm1234 -H 'Content-Type: application/json' \
  -d '{"interface":"wg0","address":"10.66.66.2/32","private_key":"...",
       "peer":{"public_key":"...","endpoint":"vpn.example:51820",
               "allowed_ips":"10.66.66.0/24","persistent_keepalive":25}}' \
  http://127.0.0.1:8080/api/wireguard
```

The device marks the tunnel `configured` when address, private key and peer
public key are all present, then applies it (Zephyr `wireguard_peer_add`). The
private key and preshared key are omitted from `GET /api/config`.

## Chat

`POST /api/chat` runs the on-device deterministic grammar (no language model):

```bash
curl -u admin:farm1234 -H 'Content-Type: application/json' \
  -d '{"text":"turn on relay 3"}' http://127.0.0.1:8080/api/chat
```

```json
{"action":"control","target":"relay","relay_id":3,"mode":"on","pulse_ms":null,
 "boost_min":null,"reply":"Setting relay 3 to on.","understood":true,"applied":true}
```

Actions are `help`, `status`, `data`, `control` and `unknown`. The `status`
action embeds the full status document. The same grammar is implemented in
`com.mraf.server.intent.IntentParser`.

## WebRTC link

`GET /api/webrtc` returns the settings the browser uses to open a direct,
encrypted data channel to the controller: the control-plane URL, the STUN URL,
the signalling `token`, the `device_id` and the suggested poll interval. When
the channel is open the dashboard routes its `/api/*` calls over it and the
Spring Boot server only relays the SDP/ICE handshake. See
[../docs/13-webrtc.md](../docs/13-webrtc.md).

## MCP

`POST /mcp` speaks MCP JSON-RPC 2.0 (Streamable HTTP, JSON response mode).
Supported methods: `initialize`, `notifications/*` (HTTP 202, empty body),
`ping`, `tools/list` and `tools/call`. Tools: `get_status`, `control_relay`,
`control_pump`, `set_all`, `apply_config`, `wireguard_status` and `chat`.

```bash
curl -u admin:farm1234 -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' http://127.0.0.1:8080/mcp
```

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
