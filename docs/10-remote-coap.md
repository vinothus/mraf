# 10 - Remote Java server over CoAP

The controller polls a remote Java server over CoAP/UDP. It asks the server
whether there is a control, data or configuration request, then reports the
result on the next poll. The server also tells the controller whether it has
been confirmed by an administrator, which profile it should use and how often
it should poll.

This is a pull protocol. The controller always initiates; the server never
connects to the controller. That keeps the farm behind NAT and avoids opening
an inbound port.

The repository includes a complete Spring Boot server with an embedded H2
database and a multi-device browser dashboard; see
[11-springboot-server.md](11-springboot-server.md).

## Settings

Open **Settings → Remote CoAP** in the ESP32 dashboard:

| Field | Default | Meaning |
|---|---|---|
| Enable remote polling | off | Turn polling on |
| Server host or IP | empty | Java server hostname or IPv4 address |
| Port | `5683` | CoAP UDP port |
| Resource path | `/mraf` | CoAP resource path |
| Poll interval | `60` | Seconds between polls, 10-3600 |
| Shared token | empty | Optional token copied into every request |
| Server user / password | empty | Login the server stores when the device registers |

The token is write-only in the REST API. A blank value in the form keeps the
existing token. The token and server credentials are stored in NVS with the
rest of the configuration.

CoAP settings apply without a reboot. Wi-Fi, hostname and login changes still
reboot the controller.

## Registration and confirmation

A device identity (`device_id`) is derived from the ESP32 MAC address and is
stable across reboots. The first polls register the device and its server
login. The server keeps the device in `pending` state until an administrator
confirms it:

1. Controller sends `register: true` with `auth.user` and `auth.pass`.
2. Server stores a password hash and returns `{"request":"register",
   "status":"pending"}`.
3. Admin confirms the device in the server dashboard.
4. The next poll returns `{"request":"register","status":"confirmed",
   "confirmed":true}` and normal requests start.
5. The device login now also works on the server dashboard, scoped to that
   device only.

A rejected device receives `status: "rejected"`. A confirmed device that
presents the wrong credentials receives `{"request":"auth","status":"denied"}`
and must fix its stored server login.

## Protocol

Every poll is a CoAP confirmable POST with a JSON body:

```json
{
  "device_id": "MRAF-A1B2C3",
  "device": "MRAF Farm Controller",
  "fw": "1.1.0",
  "token": "shared-secret",
  "time": 1793503200,
  "profile": "agri",
  "confirmed": false,
  "register": true,
  "poll_interval_sec": 60,
  "auth": {"user": "farm", "pass": "farm-pass"}
}
```

The server replies with a 2.xx response and a JSON request. Every response
includes the current meta fields:

```json
{"request": "control", "id": "MRAF-A1B2C3-ctl-1042",
 "confirmed": true, "profile": "agri", "poll_interval_sec": 60,
 "commands": [
   {"target": "pump", "mode": "on"},
   {"target": "relay", "id": 3, "mode": "on", "pulse_ms": 800}
]}
```

or:

```json
{"request": "data", "id": "MRAF-A1B2C3-data-208",
 "confirmed": true, "profile": "home", "poll_interval_sec": 120}
```

or a configuration request (abbreviated):

```json
{"request": "config", "id": "MRAF-A1B2C3-cfg-17",
 "confirmed": true, "profile": "home", "poll_interval_sec": 120,
 "channels": [
   {"id": 1, "name": "Pump House", "zone": "Utility", "equipment": "motor",
    "pulse_mode": true, "pulse_on_ms": 800, "pulse_off_ms": 600,
    "enabled": true, "days": 127, "mode": "auto",
    "windows": [{"on": "18:00", "off": "22:00"}]}
 ],
 "recurring": [
   {"name": "Weekly soak", "repeat": "weekly", "days": 1,
    "day_of_month": 1, "month": 1, "from": "05:00", "to": "05:15",
    "channel_mask": 5, "action": "on"}
 ],
 "pump": {"enabled": true, "max_run_min": 45}
}
```

or:

```json
{"request": "none", "confirmed": true, "profile": "agri", "poll_interval_sec": 60}
```

The controller adopts `confirmed`, `profile` and `poll_interval_sec` from every
response. The server can therefore change a device profile and polling
schedule without the controller exposing new inbound ports.

### Control request

The controller applies the commands immediately, saves the new modes to NVS,
and remembers the request `id` (keep ids to 32 characters or fewer; the server
generates ids such as `MRAF-A1B2C3-data-12`). On the
next poll it sends:

```json
{
  "device_id": "MRAF-A1B2C3",
  "token": "shared-secret",
  "time": 1793503230,
  "auth": {"user": "farm", "pass": "farm-pass"},
  "ack": "MRAF-A1B2C3-ctl-1042",
  "result": "applied"
}
```

`result` is `applied`, `partial`, or `rejected`. If the server sends the same
`id` again, the controller does not re-apply the command; it repeats the stored
result.

Supported control commands are the same as `POST /api/control`:

```json
{"target": "relay", "id": 1, "mode": "on"}
{"target": "relay", "id": 1, "mode": "on", "pulse_ms": 800}
{"target": "relay", "id": 1, "mode": "auto", "boost_min": 30}
{"target": "pump", "mode": "off"}
{"target": "pump", "mode": "auto", "boost_min": 60}
{"target": "all", "mode": "auto"}
{"target": "all", "mode": "off"}
```

`mode` is `auto`, `on` or `off`. `boost_min` is optional. `pulse_ms`
(0-10000; `0` uses the channel timing) turns a channel on for that duration and
is intended for momentary contactors and motor starters. The controller applies the same safety rules as the local
dashboard: dry-run, maximum run time, minimum off time, float interlocks and
outage recovery are never bypassed by a remote command.

### Data request

The controller sets a data flag. On the next poll it sends:

```json
{
  "device_id": "MRAF-A1B2C3",
  "token": "shared-secret",
  "time": 1793503230,
  "auth": {"user": "farm", "pass": "farm-pass"},
  "ack": "MRAF-A1B2C3-data-208",
  "result": "data",
  "data": {
    "now": 1793503230,
    "profile": "agri",
    "power": true,
    "battery_v": 13.41,
    "tank_full": false,
    "sump_dry": false,
    "wifi_rssi": -62,
    "ip": "192.168.1.42",
    "pump": {
      "on": true,
      "state": "RUNNING",
      "mode": "auto",
      "current_a": 6.8,
      "fault": "",
      "fault_latched": false
    },
    "relays": [1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]
  }
}
```

`relays` is in channel order 1-12. `1` means on.

### Configuration request

The controller passes the response object to the same validator used by
`POST /api/config` on the ESP32. It applies the configuration only if the whole
document is valid, then saves it to NVS and acknowledges on the next poll:

```json
{"ack": "MRAF-A1B2C3-cfg-17", "result": "applied"}
```

A rejected request uses `result: "rejected"` and leaves the running
configuration unchanged. Duplicate config IDs are not re-applied. Wi-Fi,
device identity, login and the controller's own CoAP server settings are not
sent by the Java dashboard, so an invalid remote change cannot lock the
controller out of the network.

Recurring schedules support `repeat` = `daily`, `weekly`, `monthly` or
`yearly`, an optional `days` bitmask (Monday = bit 0), `day_of_month`, `month`,
`from`/`to` in `HH:MM`, a `channel_mask` (bit 0 = relay 1) and `action` =
`on`, `off` or `auto`. Off rules win over on rules. Up to 8 recurring schedules
are accepted.

## CoAP details

- CoAP version 1, confirmable POST, 4-byte token.
- Content-Format 50 (`application/json`).
- Piggybacked response required in the same ACK; a separate response is not
  used by this client.
- Two transmissions per poll, 900 ms wait each. A lost poll is simply retried
  at the next interval.
- The ESP32 request buffer is 4096 bytes and the response buffer is 8192 bytes.
  The Spring Boot server raises Californium's `MAX_MESSAGE_SIZE`,
  `MAX_RESOURCE_BODY_SIZE` and `UDP_DATAGRAM_SIZE` to 8192 so large data
  snapshots and recurring schedules fit one datagram.
- The client uses no DTLS. On a shared or public network, run the Java server
  behind a VPN or use a private APN/LAN. The token is not encryption.

## Server behaviour

Any CoAP server that handles POST `/mraf` and returns JSON works. The included
Spring Boot server follows this sequence:

1. Read the request JSON and verify `token`.
2. Find or create the device by `device_id` and persist it to H2.
3. If the device is rejected, return `register/rejected`.
4. If the device is not confirmed, store its server login and return
   `register/pending`.
5. Verify `auth.user` and `auth.pass` for confirmed devices.
6. Store any `data` snapshot and process any `ack` result.
7. If no queued request exists, return `{"request":"none"}`.
8. Otherwise return the in-flight request, or promote the oldest queued request
   to in-flight, and append the current `confirmed`, `profile` and
   `poll_interval_sec` meta fields.

Only one request is in flight at a time. The same request is resent until it is
acknowledged. Control and config requests update the dashboard optimistically;
the ACK confirms or reports rejection.

The controller does not need a persistent connection and does not need the
server to be reachable during an outage; it will retry at the configured
interval.

## Dashboard status

The ESP32 dashboard **Remote CoAP** tile shows:

- `Off` when disabled;
- `Waiting` before the first successful poll;
- `Online` after a successful poll;
- `Pending` until an admin confirms the device;
- the host/port and the last error, if any.

`GET /api/status` exposes the same information under `remote`, including
`confirmed`, `profile`, `poll_interval_sec`, `device_id` and `user`.
