# MRAF Farm Controller

An ESP32 farm automation controller for a 5 HP 3-phase borewell pump plus 12
scheduled single-phase loads. It uses off-the-shelf modules, a built-in web
dashboard, a REST API, and a native simulator that runs on Linux without any
hardware.

The design target is a small farm or livestock shed in Tamil Nadu: 415 V /
50 Hz three-phase supply, Chennai-market parts, and a controller that stays
alive on a 12.8 V LiFePO4 battery while all relay coils drop out during a power cut.

## What it does

- 12 independent relay channels with up to 4 on/off windows per day, day masks,
  manual override and timed boost.
- Dedicated 5 HP 3-phase pump output through a DOL contactor, not through the
  relay board's 10 A trace.
- Pump protections: minimum off time, maximum run time, dry-run/no-current
  trip, tank-full and sump-dry interlocks, power-loss detection, recovery
  delay, and optional resume after outage.
- Hardwired safety chain: E-stop, thermal overload, phase-failure relay, and
  float switches directly in the contactor coil circuit.
- Wi-Fi station mode with AP fallback, mDNS (`http://mraf.local`), HTTP Basic
  auth, NTP plus DS3231 RTC, and a responsive single-page dashboard.
- LiFePO4 controller battery, charged when mains is present; relay coils are
  on a separate mains-derived 5 V SMPS so a power cut always de-energises every
  load.
- Optional Spring Boot server over CoAP: multi-device admin fleet plus
  per-device dashboards, live data, relay/pump control, profiles, recurring
  schedules, pump protection and an embedded H2 database. New ESP32s register
  themselves, wait for admin confirmation, and acknowledge control, data and
  config requests on the next poll. The server can vary each device's profile
  and poll interval.
- Optional Matter bridge: exposes each confirmed device's relays, pump, tank
  sensor and sump sensor to Apple Home, Google Home, Alexa or Home Assistant
  without changing the ESP32 firmware.
- Native simulator with accelerated clock, fault injection, HTTP API and
  scenario presets.
- No custom PCB. Everything is DIN-rail terminals, ready-made modules and
  standard panel wiring.

## Quick start: simulator, no hardware

```bash
cd /workspace/mraf
make sim
# open http://localhost:8080
```

Useful presets:

```bash
make sim-scenario SCENARIO=pump-morning
make sim-scenario SCENARIO=relay-evening
make sim-scenario SCENARIO=outage
make sim-scenario SCENARIO=dry-run
make sim-scenario SCENARIO=tank-full
make sim-scenario SCENARIO=sump-dry
```

Run the tests:

```bash
make test
```

## Quick start: Spring Boot server

```bash
cd server
MRAF_TOKEN=demo-token MRAF_DASHBOARD_PASSWORD=change-me mvn spring-boot:run
# open http://localhost:8080 (login admin / change-me)
```

Without an ESP32, run the simulated controller in a second terminal:

```bash
MRAF_TOKEN=demo-token python3 scripts/demo_controller.py --interval 1 \
  --device-id SIM-000001 --user farm --password farm1234
```

The simulated controller registers as pending; confirm it from the admin fleet
view or with:

```bash
curl -u admin:mraf-demo -H 'Content-Type: application/json' \
  -d '{}' http://localhost:8080/api/devices/SIM-000001/confirm
```

See [docs/11-springboot-server.md](docs/11-springboot-server.md). A recorded
walkthrough is at
[docs/demo/mraf-springboot-demo.mp4](docs/demo/mraf-springboot-demo.mp4).

## Quick start: Matter bridge

The bridge is optional and needs Node.js 20.19 or newer. It maps the server's
confirmed devices to Matter on/off endpoints and two contact sensors:

```bash
cd matter
npm install
MRAF_SERVER_URL=http://127.0.0.1:8080 \
MRAF_SERVER_USER=admin \
MRAF_SERVER_PASSWORD=mraf-demo \
npm start
```

Scan the printed QR code from Apple Home, Google Home, Alexa or Home Assistant.
The ESP32 firmware and CoAP protocol stay unchanged. See
[docs/12-matter-bridge.md](docs/12-matter-bridge.md) for endpoint mapping,
pulse relays, configuration and troubleshooting.

## Quick start: Zephyr firmware

The production firmware targets Zephyr RTOS and builds for both the ESP32 and a
native Linux simulator:

```bash
make zephyr-build-native
make zephyr-e2e          # build native_sim and run REST E2E tests
make zephyr-build-esp32  # cross-compile for ESP32
make zephyr-flash PORT=/dev/ttyUSB0
make zephyr-monitor PORT=/dev/ttyUSB0
```

The native build serves the dashboard on `http://127.0.0.1:8080`. ESP32 builds
need the Espressif HAL blobs once:

```bash
cd /opt/zephyrproject && west blobs fetch hal_espressif
```

See [okf/zephyr-port.md](okf/zephyr-port.md) for the architecture and
[okf/testing.md](okf/testing.md) for the full build and test guide. A short
recorded walkthrough is at
[docs/demo/zephyr-native-demo.mp4](docs/demo/zephyr-native-demo.mp4).

## Quick start: Arduino reference firmware

```bash
./scripts/setup_toolchain.sh
make compile
make flash PORT=/dev/ttyUSB0
make monitor PORT=/dev/ttyUSB0
```

The ESP32 core and ArduinoJson are installed under `/opt/mraf-tools` by default
(override with `MRAF_TOOLS=/path`). The compile step first runs
`tools/gen_webui.py`, which embeds `web/index.html` into
`firmware/mraf/webui.h`.

First boot with no Wi-Fi credentials starts an access point:

- SSID: `MRAF-xxxxxx` (last 3 bytes of the MAC)
- Password: `farm1234`
- Dashboard: `http://192.168.4.1`

Default dashboard login is `admin` / `farm1234`. Change it in Settings.
The dashboard can scan for Wi-Fi networks and store the SSID/password; changing
Wi-Fi or auth settings reboots the ESP32 automatically.

## Hardware at a glance

| Block | Part |
|---|---|
| Controller | ESP32 DevKit V1 38-pin (ESP32-WROOM-32) |
| Relay board | 16-channel 5 V active-LOW module, JD-VCC jumper removed |
| Controller power | 12.8 V 6 Ah LiFePO4 + LiFePO4 charger + LVD + 5 V buck |
| Relay power | 230 V AC to 5 V 3 A SMPS |
| Pump switching | 9 A AC-3 contactor + 5.5-8 A thermal overload |
| Pump protection | 3-pole MCB, RCCB, phase-failure relay, E-stop |
| Sensors | 4 floats (2 safety + 2 sense), ACS712-30 A, mains and battery dividers |
| Time | DS3231 RTC (CR2032) + NTP |

Key pin map:

| Function | GPIO |
|---|---|
| Relay channels 1-12 | 13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27 |
| Pump relay | 33 |
| Mains sense | 32 |
| Tank full | 34 |
| Sump dry | 35 |
| Battery sense | 36 |
| ACS712 current | 39 |
| DS3231 SDA / SCL | 4 / 5 |
| Status LED | 2 |

Full wiring, contactor and panel diagrams are in `hardware/diagrams/`.
Approximate BOM: about **₹15,600** for the controller plus pump starter, or
about **₹18,100** including the recommended RCCB and phase-failure relay.
Prices vary by shop and month; see `hardware/bom.csv`.

## Safety

This project switches lethal 230/415 V AC. It is not a certified product.

- Use a licensed electrician for the mains, contactor and pump wiring.
- Install an RCCB, a motor-rated MCB/MPCB, a phase-failure relay, a thermal
  overload and a lockable isolator.
- The E-stop and float switches must be hardwired in the contactor coil
  circuit. Firmware is not a safety layer.
- The battery only backs up the ESP32, RTC and sensors. The pump and relay
  coils are not battery-backed. Do not attempt to run the pump from the 12 V
  battery or an inverter without a proper transfer scheme.
- Keep 230/415 V wiring in a separate duct from signal wiring. Use ferrules,
  cable glands, earth bonding and covered terminals.
- Test with the pump disconnected before connecting the motor.

## Repository layout

```text
firmware/zephyr/       Zephyr firmware (production target)
firmware/mraf/         Arduino-ESP32 reference firmware
web/index.html         shared dashboard used by firmware and simulator
server/                Spring Boot CoAP server and browser dashboard
matter/                optional Node.js Matter bridge for the server
simulator/             native Python digital twin and tests
tests/                 native C++ schedule tests
tools/                 webui generator and SVG diagram generator
hardware/bom.csv       Chennai-market bill of materials
hardware/diagrams/     system, control, starter and panel SVG diagrams
hardware/wokwi/        simplified Wokwi browser check
shared/                JSON config schema
scripts/               toolchain, compile, simulator and test helpers
docs/                  requirements, wiring, firmware, API, operation, video
```

## Documentation

1. [Requirements and scope](docs/01-requirements.md)
2. [Hardware and BOM](docs/02-hardware-bom.md)
3. [Wiring and panel build](docs/03-wiring.md)
4. [Firmware architecture](docs/04-firmware.md)
5. [Build, flash and first boot](docs/05-build-flash.md)
6. [Operation and troubleshooting](docs/06-operation.md)
7. [REST API](docs/07-api.md)
8. [Simulator](docs/08-simulator.md)
9. [Assembly video script](docs/09-video-script.md)
10. [Remote Java server over CoAP](docs/10-remote-coap.md)
11. [Spring Boot server and dashboard](docs/11-springboot-server.md)
12. [Matter bridge](docs/12-matter-bridge.md)
13. [OKF knowledge base](okf/index.md)

## Verified in this workspace

- `make test`: C++ schedule/controller tests (including recurring and pulse
  cases) and 25 Python simulator/API tests pass.
- `make server-test`: 10 Spring Boot tests pass against in-memory H2
  (registration, confirmation, credentials, data ACK, control, config,
  recurring, admin actions, device-user lookup, persistence reload).
- Firmware compiles with Arduino-ESP32 3.3.12 and ArduinoJson 7.4.3:
  1,149,018 bytes (87% of the default 1,310,720-byte app partition), 72,268
  bytes of RAM (22%).
- Zephyr native_sim firmware builds and serves the dashboard, status API and
  simulator controls.
- Zephyr ESP32 firmware builds with Wi-Fi, ADC, DS3231, NVS settings and mDNS.
- `make zephyr-e2e`: 11 REST end-to-end tests pass against the Zephyr native
  firmware.
- Native simulator serves the dashboard, status API and pump scenario.
- Matter bridge mapping tests pass (7 tests), and startup against the running
  server exposes 16 endpoints for `SIM-000001` (12 relays, pump, tank and sump).
  A Matter-side relay command was ACKed by the simulated controller.
