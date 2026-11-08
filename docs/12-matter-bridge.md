# Matter bridge

The MRAF server can expose its devices to Apple Home, Google Home, Amazon
Alexa and Home Assistant through a small Node.js Matter bridge. The bridge is
optional. It does not replace the ESP32 firmware, the CoAP protocol, the MRAF
dashboard or the pump protection logic.

```text
Matter controller (Apple/Google/Alexa/Home Assistant)
        │  Matter over IPv6 + mDNS
        ▼
matter/  Node.js bridge  ── REST/HTTP Basic ──►  Spring Boot server
                                                      │ CoAP
                                                      ▼
                                                ESP32 controllers
```

Why a bridge instead of native Matter on the ESP32?

- The current firmware is built on Arduino-ESP32 and the Matter stack would add
  significant flash/RAM pressure and a second commissioning model.
- The server already owns device identity, confirmation, profiles, schedules
  and the control queue. A bridge keeps one source of truth.
- Existing installations can add Matter without reflashing every controller.

The trade-off is that the bridge host must stay running, and the bridge is not
a certified Matter product. Schedules and pump protection remain in MRAF; the
Matter controller only sees on/off endpoints and two contact sensors.

## Requirements

- Node.js **20.19 or newer** (`node --version`). The bridge refuses to start on
  older Node releases.
- A running MRAF server, for example `http://127.0.0.1:8080`.
- Server admin credentials. The bridge reads `/api/devices` and
  `/api/devices/{id}`, and posts control commands. A device user receives 403.
- IPv6 and mDNS on the bridge host. Most Matter commissioning apps discover
  the bridge over mDNS.
- A Matter controller on the same LAN.

## Install and run

```bash
cd matter
npm install
MRAF_SERVER_URL=http://127.0.0.1:8080 \
MRAF_SERVER_USER=admin \
MRAF_SERVER_PASSWORD=mraf-demo \
npm start
```

The process prints a QR code, a QR payload and a manual pairing code. Leave it
running while you commission and use the devices.

If you already have a server and simulated controller running, the bridge will
discover the confirmed device on its first poll:

```text
[matter] discovered confirmed device SIM-000001
[matter] SIM-000001: exposed relay 1 (light) as light
...
[matter] SIM-000001: exposed pump
[matter] SIM-000001: exposed tank sensor
[matter] SIM-000001: exposed sump sensor
```

The bridge also exposes a single device when `MRAF_MATTER_DEVICE` is set:

```bash
MRAF_MATTER_DEVICE=SIM-000001 npm start
```

Use `npm run check` for a syntax check and `npm test` for the mapping tests.

### Node 18 hosts

This workspace ships Node 18 by default. Matter.js 0.18 needs Node 20.19+ for
some dependencies. Use Node 22 (for example `nvm use 22`) or, in this
workspace, put the downloaded runtime first on `PATH`:

```bash
export PATH=/tmp/node22/bin:$PATH
cd matter
npm install
npm start
```

## Commissioning

1. Start the bridge and leave it running.
2. Open the Matter controller's "Add device" / "Add Matter device" flow.
3. Scan the QR code printed by the bridge, or enter the manual pairing code.
4. The controller adds the **MRAF Matter Bridge** aggregator. Its bridged
   devices appear after the controller reads the endpoints.

The default development credentials are:

| Setting | Default |
|---|---|
| Passcode | `20202021` |
| Discriminator | `3840` |
| Matter UDP port | `5540` |

Change the passcode and discriminator with `MRAF_MATTER_PASSCODE` and
`MRAF_MATTER_DISCRIMINATOR` before pairing a real installation. The pairing
code is printed only while the bridge is uncommissioned. If the bridge is
already commissioned, remove its storage directory or remove it from the
controller before pairing again.

## Endpoint mapping

Each confirmed, non-rejected MRAF device becomes an aggregator child. Pending
devices are not exposed until an admin confirms them.

| MRAF source | Matter device | Notes |
|---|---|---|
| Relay with `equipment: light` | OnOff Light (0x0100) | On/off only |
| Relay with any other equipment | OnOff Plug-in Unit (0x010A) | Valve, motor, fan, pump, socket, heater, door, other |
| Pump | OnOff Plug-in Unit (0x010A) | On/off, pump protection stays in MRAF |
| `tank_full` | Contact Sensor (0x0015) | Read-only |
| `sump_dry` | Contact Sensor (0x0015) | Read-only |

Relay endpoints use stable IDs derived from the MRAF device ID and relay
number, for example `mraf-SIM-000001-b3746f-r1`. Labels include the device,
zone and relay name, truncated to the Matter label limit.

Only 12 relays, one pump and two sensors are mapped. The bridge does not
expose schedules, profiles, battery voltage, current, power, RSSI or arbitrary
configuration. Use the MRAF dashboard or REST API for those.

### Pulse relays

If a relay has `pulse_mode: true`, the bridge sends the channel's pulse timing
with the control command. A Matter "on" or "off" therefore triggers a pulse
rather than a latched output. The endpoint state is then refreshed from the
server on the next poll, so a momentary pulse may appear to turn itself off.

### Equipment changes

The Matter device type is chosen when an endpoint is created. Changing a relay
from `light` to `valve` (or the reverse) requires restarting the bridge. The
bridge logs a warning when it notices the mismatch. Adding or removing relays
also leaves old endpoints in place until the bridge storage is reset.

## State and commands

- The bridge polls `/api/devices` every 5 seconds and refreshes each device's
  config from `/api/devices/{id}` every 60 seconds.
- Relay and pump state is read from the latest `lastData` snapshot. Sensors use
  `tank_full` and `sump_dry`.
- A Matter command posts to `/api/devices/{id}/control`:
  - relay: `{"target":"relay","id":3,"mode":"on"}` plus `pulse_ms` for pulse
    channels;
  - pump: `{"target":"pump","mode":"on"}`.
- The server queues the command for the next ESP32 poll. The Matter endpoint
  updates optimistically, then the bridge ignores stale poll results for 8
  seconds so the command is not immediately overwritten.
- If the HTTP request fails, the bridge reverts the Matter state and logs the
  error.
- A device that stops polling is marked unreachable, but its last state is
  retained.

The command path was verified with the simulated controller: a Matter-side
relay command produced a server control ACK (`applied`) on the next poll.
Startup state sync does not enqueue commands.

## Configuration reference

| Variable | Default | Purpose |
|---|---|---|
| `MRAF_SERVER_URL` | `http://127.0.0.1:8080` | Server base URL |
| `MRAF_SERVER_USER` | `admin` | Server login |
| `MRAF_SERVER_PASSWORD` | `mraf-demo` | Server password |
| `MRAF_MATTER_DEVICE` | all confirmed | Limit the bridge to one device ID |
| `MRAF_MATTER_PORT` | `5540` | Matter UDP port |
| `MRAF_MATTER_STORAGE` | `data/matter` | Matter node/fabric storage |
| `MRAF_MATTER_POLL_MS` | `5000` | State poll interval |
| `MRAF_MATTER_CONFIG_MS` | `60000` | Config refresh interval |
| `MRAF_MATTER_COMMAND_GRACE_MS` | `8000` | State-sync grace after a command |
| `MRAF_MATTER_PASSCODE` | `20202021` | Commissioning passcode |
| `MRAF_MATTER_DISCRIMINATOR` | `3840` | Commissioning discriminator |
| `MRAF_MATTER_VENDOR_ID` | `0xfff1` | Matter vendor ID |
| `MRAF_MATTER_PRODUCT_ID` | `0x8001` | Matter product ID |
| `MRAF_MATTER_NODE_ID` | `mraf-bridge` | Matter node ID |
| `MRAF_MATTER_LOG_LEVEL` | Matter.js default | `debug`, `info`, `warn` |

Do not commit real passwords. Prefer a dedicated server admin account and
environment variables or your service manager's secret store.

## Service example

A minimal systemd unit for a Linux bridge host:

```ini
[Unit]
Description=MRAF Matter bridge
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=/opt/mraf/matter
Environment=MRAF_SERVER_URL=http://127.0.0.1:8080
Environment=MRAF_SERVER_USER=admin
Environment=MRAF_SERVER_PASSWORD=change-me
ExecStart=/usr/bin/node src/mraf-matter-bridge.mjs
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

The storage directory (`data/matter` by default) contains the Matter fabric and
commissioning state. Back it up if you do not want to re-pair after moving the
bridge.

## Troubleshooting

| Symptom | Check |
|---|---|
| `MRAF Matter bridge needs Node.js 20.19` | Use Node 20.19+ or Node 22 |
| HTTP 401/403 during polling | `MRAF_SERVER_USER` / `MRAF_SERVER_PASSWORD`; must be an admin |
| No devices appear | Device must be confirmed and not rejected; check `GET /api/devices` |
| No QR code | Bridge already commissioned; remove storage or unpair it first |
| Controller cannot discover bridge | IPv6 and mDNS on the bridge host; UDP 5540 reachable |
| Commands do nothing | Device offline, server queue not polled, or relay is in pulse mode |
| `storage lock` error | Another bridge process is using the same storage directory |
| Equipment type looks wrong after a config change | Restart the bridge; reset storage if old endpoints must disappear |

## Verification in this workspace

- `npm test`: 7 mapping tests pass.
- `npm run check`: both source files pass `node --check`.
- Startup against the running server exposed 16 endpoints for `SIM-000001`:
  12 relays (3 lights, 9 plug-in units), 1 pump and 2 contact sensors.
- Restarting with the same storage recreated the same endpoints and kept the
  bridge commissioning state.
- A Matter-side relay command reached the server control queue and was ACKed
  by the simulated controller; startup state sync enqueued no commands.

Commissioning against a physical Apple, Google, Alexa or Home Assistant
controller was not performed in this workspace.
