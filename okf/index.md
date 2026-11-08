---
okf: 1
title: MRAF Farm Controller
type: index
updated: 2026-11-08
tags: [mraf, esp32, zephyr, irrigation, pump, rest]
---

# MRAF Farm Controller - knowledge index

MRAF is an ESP32 farm controller for twelve scheduled single-phase loads and a
5 HP three-phase borewell pump. The production firmware now targets Zephyr
RTOS. The original Arduino sketch is kept as a reference implementation, and
the dashboard, REST API, simulator and CoAP server protocol are shared.

## Documents

| Document | Contents |
|---|---|
| [project.md](project.md) | Product scope, hardware map, safety model and repository layout |
| [zephyr-port.md](zephyr-port.md) | Zephyr architecture, configuration, board overlays and migration notes |
| [api.md](api.md) | REST endpoints, authentication, request and response shapes |
| [testing.md](testing.md) | Build, native E2E tests, simulator tests and verification results |

## Canonical commands

```bash
make zephyr-build-native   # build native_sim firmware
make zephyr-e2e            # build and run REST E2E tests
make zephyr-build-esp32    # build ESP32 firmware
make zephyr-flash PORT=/dev/ttyUSB0
```

## Status

- Native `native_sim/native/64` firmware builds and serves the dashboard and
  REST API.
- ESP32 `esp32_devkitc/esp32/procpu` firmware builds with Wi-Fi, RTC, ADC,
  NVS settings and mDNS enabled.
- The Python simulator remains the reference for demo scenarios and UI work.
- Existing Spring Boot CoAP server and Matter bridge integrations are
  unchanged.
