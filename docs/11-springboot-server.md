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
- **Activity** tab: ACKs, events, queue inspection and cancellation.

Live data, ACK history and events are in memory and reset when the server
restarts. Device registrations, credentials, configs, the last data snapshot
and pending requests are persisted in H2.

Wi-Fi credentials, device identity, dashboard login and the controller's CoAP
server settings stay local to the ESP32. They are deliberately not editable
from the remote server, because a bad remote change could lock the controller
out of its own network.

## HTTP API

All endpoints except `/api/health` require HTTP Basic auth.

| Method | Path | Role | Purpose |
|---|---|---|---|
| `GET` | `/api/health` | public | Liveness, device count and any-online flag |
| `GET` | `/api/me` | any | Current username, role and owned device |
| `GET` | `/api/devices` | admin | Fleet overview |
| `GET` | `/api/devices/{id}` | admin or owner | Full device state, config, queue, ACKs, events |
| `POST` | `/api/devices/{id}/data-request` | admin or owner | Queue a fresh snapshot |
| `POST` | `/api/devices/{id}/control` | admin or owner | Queue relay/pump control |
| `POST` | `/api/devices/{id}/config` | admin or owner | Validate and queue a full config |
| `DELETE` | `/api/devices/{id}/queue` | admin or owner | Cancel queued requests |
| `POST` | `/api/devices/{id}/confirm` | admin | Confirm a pending device |
| `POST` | `/api/devices/{id}/reject` | admin | Reject a pending device |
| `POST` | `/api/devices/{id}/update` | admin | Change profile / poll interval |
| `DELETE` | `/api/devices/{id}` | admin | Delete a device and its requests |
| `GET` | `/api/state` | any | Backwards-compatible alias for the fleet or own device |

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

A recorded walkthrough is in
[demo/mraf-springboot-demo.mp4](demo/mraf-springboot-demo.mp4). The recording
script now confirms the pending simulator and opens its device dashboard before
showing telemetry, control, schedules and activity.

The browser recording is reproducible with Playwright:

```bash
cd /tmp
mkdir -p mraf-video && cd mraf-video
npm init -y
npm i playwright@1.55.0
npx playwright install chromium
MRAF_VIDEO_DIR=/tmp/mraf-video/raw \
  node /workspace/mraf/scripts/record_server_demo.js
```

The script writes a WebM file under `MRAF_VIDEO_DIR`; encode to MP4 with
`ffmpeg -i raw.webm -c:v libx264 -pix_fmt yuv420p -movflags +faststart demo.mp4`.

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
