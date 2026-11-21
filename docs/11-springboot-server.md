# 11 - Spring Boot server and dashboard

The `server/` directory is a Spring Boot application that speaks the CoAP
contract in [10-remote-coap.md](10-remote-coap.md). It manages a fleet of
ESP32 controllers, stores registrations, configurations and queued requests in
an embedded H2 database, and serves an admin dashboard plus a per-device
dashboard.

The controller still owns the hardware safety rules. A remote schedule or
control command goes through the same controller validation as the local
ESP32 dashboard.

## Requirements

- Java 21
- Maven 3.9+
- UDP port `5683` reachable from the ESP32s
- TCP port `8080` for the browser dashboard

## Run

```bash
cd server
MRAF_TOKEN='replace-with-a-long-shared-token' \
MRAF_DASHBOARD_PASSWORD='replace-this-password' \
mvn spring-boot:run
```

Or build once and run the jar:

```bash
cd server
mvn -DskipTests package
MRAF_TOKEN='replace-with-a-long-shared-token' \
MRAF_DASHBOARD_PASSWORD='replace-this-password' \
java -jar target/mraf-server-1.0.0.jar
```

Open <http://localhost:8080>. The default demo admin login is `admin` /
`mraf-demo`; change it with `MRAF_DASHBOARD_PASSWORD` before exposing the port.

| Environment variable | Default | Meaning |
|---|---|---|
| `MRAF_TOKEN` | `demo-token` | Shared token in every CoAP request; blank accepts any |
| `MRAF_HTTP_PORT` | `8080` | Dashboard/API HTTP port |
| `MRAF_COAP_PORT` | `5683` | CoAP UDP port |
| `MRAF_COAP_PATH` | `/mraf` | CoAP resource path |
| `MRAF_DASHBOARD_USER` | `admin` | Admin Basic-auth user |
| `MRAF_DASHBOARD_PASSWORD` | `mraf-demo` | Admin Basic-auth password |
| `MRAF_TIMEZONE` | `Asia/Kolkata` | Timezone shown in the dashboard |
| `MRAF_DB_URL` | `jdbc:h2:file:./data/mraf;AUTO_SERVER=TRUE` | Embedded H2 database URL |
| `MRAF_DB_USER` | `sa` | H2 user |
| `MRAF_DB_PASSWORD` | empty | H2 password |
| `MRAF_H2_CONSOLE` | `false` | Enable the H2 web console |

The H2 file lives under `server/data/` by default. Hibernate creates and
updates the `devices` and `device_requests` tables automatically
(`spring.jpa.hibernate.ddl-auto=update`). Delete `server/data/` to start from a
clean fleet.

## ESP32 settings

Open the ESP32 dashboard **Settings → Remote CoAP** and set:

| Field | Value |
|---|---|
| Enable remote polling | on |
| Server host or IP | LAN address of the machine running Spring Boot |
| Port | `5683` |
| Resource path | `/mraf` |
| Poll interval | `10`-`30` seconds recommended |
| Shared token | same value as `MRAF_TOKEN` |
| Server user / password | a unique login for this device |

The Java server never connects inbound to the controller. The controller polls
out, so it can sit behind NAT. The server marks a controller offline after
90 seconds without a poll.

## Registration and roles

1. A new ESP32 polls with `device_id`, `profile`, `register: true` and its
   chosen server login. It appears in the admin fleet as **pending**.
2. An admin confirms or rejects it from the fleet view. Confirming stores the
   device profile and polling interval.
3. The confirmed device starts receiving data/config/control requests.
4. The device's server login now authenticates against the dashboard and can
   open only its own device.

The admin account can see every device, confirm/reject/delete devices, change
profiles and polling intervals, and open any device dashboard. Device users can
view their own dashboard, queue control/config/data requests, and clear their
own queue. Admin-only endpoints return HTTP 403 for device users.

## Dashboard

The dashboard is a single-page app served from `static/`:

- **Fleet** (admin only): counts of total, confirmed, online and pending
  devices; one card per controller with online state, profile, firmware, last
  poll and queue depth; inline confirm/reject actions.
- **Dashboard** tab: live battery, mains, pump, floats, active outputs, CoAP
  queue and link state; relay and pump controls; animated equipment icons
  (motor/fan spin, lights glow, valves and pumps rotate, heaters glow, doors
  swing, sockets pulse); per-channel **Pulse** buttons when momentary mode is
  enabled.
- **Schedules** tab: device profile, server-varied poll interval, per-channel
  name, zone, equipment type, enable, mode, day mask, up to four daily windows
  and pulse timing; pump dry-run, maximum-run, minimum-off, outage recovery and
  tank/sump interlocks. Saving validates and queues a `config` request.
- **Recurring** tab: up to eight daily/weekly/monthly/yearly schedules with
  channel selection, action and time window.
- **Tunnel** tab: WireGuard overlay state, server key and listen port, the peer
  list with handshake and online state, this device's rendered client config and
  provision / apply-to-kernel actions. A peer is auto-provisioned when a device
  is confirmed.
- **Assistant** tab: offline grammar chat (the same deterministic parser the
  ESP32 runs) with quick-command buttons; commands are queued over CoAP.
- **Activity** tab: ACKs, events, queue inspection and cancellation.
- **Settings** tab (admin only): dashboard branding. The heading and subheading
  are stored in the `server_settings` table and are also shown on the sign-in
  card, in the app header and in the page title. Blank fields restore the
  default `நீர்காவலன் Neer Kaavalan` / `நுண் வேளாண்மை கட்டுப்பாட்டு மையம் –
  Smart Agriculture Control Center` text. The device dashboard exposes the same
  two fields in its own Settings page (`device.heading` and
  `device.subheading`); an empty device subheading falls back to the device
  name.
- **Theme**: a header button switches the whole dashboard between the dark and
  bright (light) themes; the choice is stored in `localStorage` and applied
  before first paint so reloads do not flash the other theme.

Live data, ACK history and events are in memory and reset when the server
restarts. Device registrations, credentials, configs, the last data snapshot
and pending requests are persisted in H2.

Wi-Fi credentials, device identity, dashboard login and the controller's CoAP
server settings stay local to the ESP32. They are deliberately not editable
from the remote server, because a bad remote change could lock the controller
out of its own network.

## HTTP API

All endpoints except `/api/health` and `GET /api/branding` require HTTP Basic
auth. Branding is public so the sign-in card can render it before the user
authenticates.

| Method | Path | Role | Purpose |
|---|---|---|---|
| `GET` | `/api/health` | public | Liveness, device count and any-online flag |
| `GET` | `/api/branding` | public | Dashboard heading and subheading |
| `PUT` | `/api/branding` | admin | Update heading and subheading (blank = defaults) |
| `GET` | `/api/me` | any | Current username, role and owned device |
| `GET` | `/api/devices` | admin | Fleet overview |
| `GET` | `/api/devices/{id}` | admin or owner | Full device state, config, queue, ACKs, events |
| `POST` | `/api/devices/{id}/data-request` | admin or owner | Queue a fresh snapshot |
| `POST` | `/api/devices/{id}/control` | admin or owner | Queue relay/pump control |
| `POST` | `/api/devices/{id}/config` | admin or owner | Validate and queue a full config |
| `GET` | `/api/devices/{id}/console` | admin or owner | Buffered on-device console output |
| `POST` | `/api/devices/{id}/console` | admin or owner | Queue a shell command (`command`) |
| `DELETE` | `/api/devices/{id}/console` | admin or owner | Clear the console buffer |
| `DELETE` | `/api/devices/{id}/queue` | admin or owner | Cancel queued requests |
| `POST` | `/api/devices/{id}/confirm` | admin | Confirm a pending device |
| `POST` | `/api/devices/{id}/reject` | admin | Reject a pending device |
| `POST` | `/api/devices/{id}/update` | admin | Change profile / poll interval |
| `DELETE` | `/api/devices/{id}` | admin | Delete a device and its requests |
| `POST` | `/api/chat` | admin or owner | Grammar chat (`device_id` + `text`) |
| `POST` | `/mcp` | admin | MCP JSON-RPC 2.0 endpoint (tools, wireguard, console, chat) |
| `GET` | `/api/wireguard/status` | admin | Overlay status, server key and listen port |
| `GET` | `/api/wireguard/peers` | admin | Provisioned peers and handshake state |
| `POST` | `/api/wireguard/devices/{id}/peer` | admin | Provision or replace a device peer |
| `GET` | `/api/wireguard/devices/{id}/config/wg` | admin or owner | Rendered client config |
| `DELETE` | `/api/wireguard/devices/{id}/peer` | admin | Remove a device peer |
| `POST` | `/api/wireguard/apply` | admin | Apply the interface to the kernel (when enabled) |
| `GET` | `/api/state` | any | Backwards-compatible alias for the fleet or own device |

`POST /api/devices/{id}/config` accepts the same `relay_count` as the
firmware/simulator: 1-12, with exactly that many `channels`. The server dashboard
shows only those relays, and its Profiles & schedules tab can add or delete
relay records; relay controls above `relay_count` are rejected.

Example control with a momentary pulse:

```bash
curl -u admin:mraf-demo \
  -H 'Content-Type: application/json' \
  -d '{"target":"relay","id":3,"mode":"on","pulse_ms":800}' \
  http://localhost:8080/api/devices/MRAF-A1B2C3/control
```

Example device login:

```bash
curl -u farm:farm-pass \
  http://localhost:8080/api/devices/MRAF-A1B2C3
```

## CoAP queue behaviour

- One request is in flight at a time per device.
- The same request is resent until the controller acknowledges its `id`.
- Control and configuration changes update the dashboard optimistically, then
  the ACK confirms or reports rejection.
- The server auto-queues one data request when a confirmed device has no
  snapshot yet.
- Duplicate control/config IDs are not re-applied by the controller.
- Californium `MAX_MESSAGE_SIZE`, `MAX_RESOURCE_BODY_SIZE` and
  `UDP_DATAGRAM_SIZE` are raised to 8192 so full configurations and data
  snapshots fit in one datagram.

The wire contract is in [10-remote-coap.md](10-remote-coap.md).

## Remote console

The dashboard's **Console** tab lists every connected controller. Selecting one
attaches to its on-device Zephyr shell: the server queues a `console` request
with a single command line, the controller runs it on its next poll and returns
the captured output in the ACK. Output is kept per device (last 200 entries) and
shown as a terminal with the `uart:~$` prompt.

```bash
# run a command on the controller's shell
curl -u admin:mraf-demo -H 'Content-Type: application/json' \
  -d '{"command":"mraf status"}' \
  http://localhost:8080/api/devices/SIM-000001/console

# read the buffered output
curl -u admin:mraf-demo http://localhost:8080/api/devices/SIM-000001/console
```

The same capability is exposed to MCP clients as the `console_command` and
`console_output` tools. Typical commands are `help`, `kernel version`,
`device list`, `net iface`, `uptime`, `threads`, `mraf status`,
`mraf relay <1-relay_count> <on|off|auto>`, `mraf pump <on|off|auto>` and `mraf wg`.
Relay and pump commands from the console go through the same safety rules as the
dashboard.

## Tests and demo

```bash
make server-test
make server-build
make server-run
```

The server tests start a Spring context with an in-memory H2 database and cover
registration, confirmation, credential rejection, data ACKs, control,
configuration validation, recurring schedules, admin actions, device-user
lookup and persistence across a reload.

The repository includes a simulated controller that speaks the same CoAP
contract:

```bash
# terminal 1
make server-run

# terminal 2
MRAF_TOKEN=demo-token python3 scripts/demo_controller.py --interval 1 \
  --device-id SIM-000001 --user farm --password farm1234
```

The simulated controller registers, waits for admin confirmation, then applies
queued control/config requests, sends snapshots and evaluates its own schedules
and recurring rules. Use the admin dashboard to confirm it, or:

```bash
curl -u admin:mraf-demo -H 'Content-Type: application/json' \
  -d '{}' http://localhost:8080/api/devices/SIM-000001/confirm
```

A live walkthrough in the bright theme is in
[demo/mraf-server-dashboard-demo.mp4](demo/mraf-server-dashboard-demo.mp4):
admin login, theme selection, confirmation of the pending ESP32, live relay and
pump commands, schedules, recurring rules, the activity log and the device-user
login as `farm`.

The remote console is demonstrated in
[demo/mraf-server-console-demo.mp4](demo/mraf-server-console-demo.mp4): the
Console tab lists the connected controllers, one is selected, and shell commands
(`help`, `mraf status`, `kernel version`, `net iface`, `mraf relay 5 on`,
`mraf wg`, `uptime`) are run with their output streamed back over CoAP. It is
recorded with `scripts/record_server_console_demo.js`.

The walkthrough is reproducible with Playwright:

```bash
cd /tmp
mkdir -p mraf-video && cd mraf-video
npm init -y
npm i playwright@1.55.0
npx playwright install chromium
MRAF_VIDEO_DIR=/tmp/mraf-video/raw \
  NODE_PATH=/tmp/mraf-video/node_modules \
  node /workspace/mraf/scripts/record_server_demo.js
```

The script writes a WebM file under `MRAF_VIDEO_DIR`; encode to MP4 with
`ffmpeg -i raw.webm -c:v libx264 -pix_fmt yuv420p -movflags +faststart demo.mp4`.

The configurable heading and subheading are demonstrated in
[demo/customheaderandsubheader.mp4](demo/customheaderandsubheader.mp4): the
ESP32 dashboard and the server UI side by side, starting from the defaults,
saving custom values in each Settings page, then reloading / signing in again
to show the saved text. Record it with
`scripts/record_custom_header_demo.py` against a running native firmware
(`MRAF_HTTP_PORT=18090`) and server (`MRAF_HTTP_PORT=18085`).

## Matter

The optional [Matter bridge](12-matter-bridge.md) exposes confirmed devices to
Apple Home, Google Home, Alexa and Home Assistant over Matter. It runs as a
separate Node.js 20.19+ process and uses the same admin REST API; the ESP32
firmware and CoAP server are unchanged.

## Security

- Change both the CoAP token and the dashboard password.
- Device passwords are stored as delegating-encoder hashes (bcrypt by default).
- HTTP Basic auth does not encrypt traffic. Use a trusted LAN, VPN or a TLS
  reverse proxy for remote access.
- CoAP has no DTLS in this implementation. Do not expose UDP `5683` directly
  to the public internet.
- Keep the dashboard on a private network or VPN.
