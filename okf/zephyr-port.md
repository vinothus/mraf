---
okf: 1
title: Zephyr port architecture
type: architecture
updated: 2026-11-08
tags: [zephyr, native-sim, esp32, drivers, migration]
---

# Zephyr port architecture

## Source layout

```text
firmware/zephyr/
  CMakeLists.txt
  Kconfig
  prj.conf
  sections-rom.ld
  boards/esp32_devkitc_procpu.conf
  boards/esp32_devkitc_procpu.overlay
  boards/native_sim_native_64.conf
  include/         Public headers and compatibility layer
  src/             Controller, state, API, platform backends, main
  third_party/     Vendored ArduinoJson 7
```

The controller and schedule logic are shared with the Arduino reference and
the C++ host tests. Platform-specific work is isolated in
`src/platform_native.cpp` and `src/platform_esp32.cpp`.

## Main components

| File | Responsibility |
|---|---|
| `src/main.cpp` | Boot sequence, device ID, RTC/clock init, network/API start, 10 ms service loop |
| `src/controller.cpp` | Relay and pump state machine, timers, event log |
| `src/state.cpp` | Shared config/runtime state, virtual clock, status serialization, control application |
| `src/settings.cpp` | Defaults, JSON parsing, validation and persistence wrapper |
| `src/api.cpp` | Zephyr HTTP server resources, Basic auth, REST handlers |
| `src/remote.cpp` | CoAP polling thread and request acknowledgement |
| `src/coap_client.cpp` | Confirmable CoAP POST client over UDP |
| `src/platform_native.cpp` | Simulated sensors and actuators, file settings, host networking |
| `src/platform_esp32.cpp` | GPIO, ADC, DS3231, Wi-Fi AP+STA, SNTP, mDNS, NVS settings, reboot |

## Clock model

Both targets use a virtual clock:

```text
now = base_epoch + uptime * speed + offset
```

- Native starts at a simulator epoch and uses `CONFIG_MRAF_SIMULATOR`.
- ESP32 starts from the DS3231 RTC and speed 1; SNTP updates the clock and
  writes the RTC.
- `mraf_now()` is used everywhere instead of `millis()`/`k_uptime_get()` for
  scheduling decisions.
- Simulator endpoints can set the epoch, change speed, advance time and inject
  power, sensor and current faults.

## HTTP server integration

The Zephyr HTTP server uses named services and linker iterable sections.
`firmware/zephyr/sections-rom.ld` declares
`http_resource_desc_mraf_http`; `CMakeLists.txt` registers it with:

```cmake
zephyr_linker_sources(SECTIONS sections-rom.ld)
zephyr_linker_section(NAME http_resource_desc_mraf_http
                      KVMA RAM_REGION GROUP RODATA_REGION)
```

Authentication is performed in the dynamic route callback. The
`Authorization` header is captured with:

```c
HTTP_SERVER_REGISTER_HEADER_CAPTURE(capture_auth, "Authorization");
```

`MRAF_HTTP_PORT` is a Kconfig value. On POSIX builds it can be overridden at
runtime with the `MRAF_HTTP_PORT` environment variable, which the E2E tests
use.

## ESP32 devicetree and drivers

- I2C0 is remapped to GPIO 4/5 with open-drain pinctrl.
- GPIO 16/17 are released from the board's PSRAM reservation through a
  `gpio-reserved-ranges` override so they can drive relays.
- ADC1 channels 0 and 3 measure battery voltage and ACS712 current.
- The DS3231 is exposed through the MFD driver (`maxim,ds3231-mfd`) with a
  child RTC device (`maxim,ds3231-rtc`).
- Wi-Fi uses ESP32 AP+STA mode and Wi-Fi manager. mDNS and DNS-SD are enabled
  only on ESP32.

## Persistence

- Native: `CONFIG_MRAF_SETTINGS_PATH`, default `mraf-settings.json`.
- ESP32: `CONFIG_SETTINGS`, NVS backend, key `mraf/settings`.
- Settings are written as one JSON blob so the existing parser and config
  schema remain the source of truth.

## Build environment

```bash
source /opt/zephyr-venv/bin/activate
export ZEPHYR_BASE=/opt/zephyrproject/zephyr

# Native
export ZEPHYR_TOOLCHAIN_VARIANT=host
west build -s firmware/zephyr -b native_sim/native/64 -d firmware/zephyr/build/native

# ESP32
export PATH=/opt/mraf-tools/arduino15/packages/esp32/tools/esp-x32/2601/bin:$PATH
export ZEPHYR_TOOLCHAIN_VARIANT=cross-compile
export CROSS_COMPILE=/opt/mraf-tools/arduino15/packages/esp32/tools/esp-x32/2601/bin/xtensa-esp32-elf-
west build -s firmware/zephyr -b esp32_devkitc/esp32/procpu -d firmware/zephyr/build/esp32
```

`scripts/zephyr_build.sh` wraps both commands. ESP32 builds need the Espressif
HAL blobs:

```bash
cd /opt/zephyrproject
west blobs fetch hal_espressif
```

If GitHub throttles the download, retry the command. The build fails if Wi-Fi
blobs are missing because the radio driver cannot link.

## Demo

A 35-second terminal walkthrough of the native build, ESP32 build, E2E suite
and live API is at
[../docs/demo/zephyr-native-demo.mp4](../docs/demo/zephyr-native-demo.mp4).

## Migration notes

- Arduino `String` is replaced by a fixed 192-byte `String` in
  `include/mraf_compat.h`; ArduinoJson must be compiled with
  `ARDUINOJSON_ENABLE_ARDUINO_STRING=0`.
- Minimal libc has no `sscanf` or `%f`; configuration parsing is manual and
  floating point is parsed with `strtod`.
- `localtime_r` is supplied by `mraf_localtime_r`, which understands a POSIX TZ
  string such as `IST-5:30`.
- Arduino `ESP.restart()` maps to `esp_restart()` on ESP32 and
  `sys_reboot(SYS_REBOOT_COLD)` on POSIX.
- HTTP responses are serialized into fixed char buffers, not `String`.
