---
okf: 1
title: MRAF project scope and hardware
type: project
updated: 2026-11-08
tags: [hardware, scope, safety, esp32]
---

# Project scope and hardware

## What the controller does

- Runs twelve relay channels with up to four on/off windows per channel, day
  masks, manual override, pulse mode and timed boost.
- Drives a dedicated 5 HP three-phase pump contactor on GPIO 33.
- Protects the pump with minimum-off time, maximum run time, dry-run current
  detection, tank-full and sump-dry interlocks, outage recovery and optional
  resume-after-outage.
- Serves a responsive dashboard and a JSON REST API over Wi-Fi station mode,
  with AP fallback, mDNS, NTP, DS3231 RTC and HTTP Basic auth.
- Optionally polls a Spring Boot CoAP server for fleet control, data and
  configuration requests.
- Optionally exposes relays and sensors through the Node.js Matter bridge.

## Pin map

| Function | GPIO | Notes |
|---|---|---|
| Relay 1-12 | 13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27 | Active LOW |
| Pump relay | 33 | Dedicated contactor channel |
| Mains sense | 32 | Digital input |
| Tank full | 34 | Input-only pin |
| Sump dry | 35 | Input-only pin |
| Battery sense | 36 | ADC1 channel 0, 100k/22k divider |
| ACS712 current | 39 | ADC1 channel 3, 66/2 mV/A |
| DS3231 SDA / SCL | 4 / 5 | I2C0 remapped from GPIO21/22 |
| Status LED | 2 | Heartbeat and AP blink |

## Power and safety model

- Relay coils and the pump contactor are powered from a mains-derived 5 V
  supply. A mains outage drops every load.
- The ESP32, RTC and sensors run from a 12.8 V LiFePO4 battery with a charger
  and low-voltage disconnect.
- The E-stop, thermal overload, phase-failure relay and float safety contacts
  are hardwired in the contactor coil circuit. Firmware is not a safety layer.
- 230/415 V wiring must be installed and inspected by a licensed electrician.

## Repository layout

```text
firmware/zephyr/       Zephyr firmware (production target)
firmware/mraf/         Arduino-ESP32 reference implementation
web/index.html         Shared dashboard embedded in firmware
simulator/             Python digital twin and API tests
tests/                 C++ controller tests and Zephyr E2E tests
server/                Spring Boot CoAP server and fleet dashboard
matter/                Optional Matter bridge
tools/                 webui.h and diagram generators
hardware/              BOM, SVG diagrams and Wokwi project
docs/                  Human documentation and video script
okf/                   This knowledge base
```

## Configuration model

The runtime configuration is a fixed-size C++ struct. It is persisted as JSON:

- Native firmware writes `mraf-settings.json` in the working directory.
- ESP32 firmware stores the JSON blob in Zephyr settings/NVS under
  `mraf/settings`.

Changing Wi-Fi, hostname or dashboard credentials requires a reboot. Relay,
pump, schedule and timezone changes apply immediately.
