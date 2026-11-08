---
okf: 1
title: Build and test guide
type: testing
updated: 2026-11-08
tags: [build, test, e2e, zephyr, simulator, verification]
---

# Build and test guide

## Prerequisites

- Zephyr 4.5 at `/opt/zephyrproject/zephyr`
- Python virtual environment at `/opt/zephyr-venv` with `west`
- ESP32 cross toolchain:
  `/opt/mraf-tools/arduino15/packages/esp32/tools/esp-x32/2601/bin`
- esptool 5.3.1 under the same Arduino tools directory
- Espressif HAL blobs for the ESP32 build

## Build native_sim

```bash
make zephyr-build-native
```

Equivalent:

```bash
source /opt/zephyr-venv/bin/activate
export ZEPHYR_BASE=/opt/zephyrproject/zephyr
export ZEPHYR_TOOLCHAIN_VARIANT=host
west build -s firmware/zephyr -b native_sim/native/64 \
  -d firmware/zephyr/build/native
```

Output: `firmware/zephyr/build/native/zephyr/zephyr.exe`.

The native build uses the host network stack through offloaded sockets. The
HTTP server listens on port 8080 by default; set `MRAF_HTTP_PORT` to override
the port at runtime.

## Run native firmware

```bash
make zephyr-run
# or
./firmware/zephyr/build/native/zephyr/zephyr.exe
```

Open `http://127.0.0.1:8080` and log in with `admin` / `farm1234`.

## E2E tests

```bash
make zephyr-e2e
```

The test launches the native binary in a temporary directory, on port 18080,
and checks:

- public `/health` and Basic-auth enforcement
- dashboard HTML delivery
- configuration GET/POST round trip
- relay and pump control
- time set and validation
- simulator power, sensor, current, speed and step controls
- events, Wi-Fi scan and invalid-control errors

Tests are in `tests/test_zephyr_e2e.py` and use only the Python standard
library.

## Build ESP32

```bash
cd /opt/zephyrproject
source /opt/zephyr-venv/bin/activate
west blobs fetch hal_espressif   # once, if not already present

cd /workspace/mraf
make zephyr-build-esp32
```

Output: `firmware/zephyr/build/esp32/zephyr/zephyr.bin`.

The ESP32 build enables:

- Wi-Fi manager, AP+STA, ESP32 Wi-Fi driver and reconnect
- mDNS responder and DNS-SD
- ADC battery/current sensing and DS3231 MFD RTC
- Zephyr settings with NVS
- SNTP time sync

## Flash and monitor

```bash
make zephyr-flash PORT=/dev/ttyUSB0
make zephyr-monitor PORT=/dev/ttyUSB0
```

If flashing fails, hold BOOT, tap EN/RST, release BOOT and retry. Use a data
USB cable and a real serial port.

## Existing tests

```bash
make test          # C++ schedule/controller tests and Python simulator tests
make server-test   # Spring Boot tests
make matter-test   # Matter bridge mapping tests
```

`make test-all` runs the firmware/simulator tests, Zephyr E2E tests and Spring
Boot tests.

## Verification record

| Check | Result |
|---|---|
| Zephyr native_sim build | Pass |
| Zephyr ESP32 build with Espressif blobs | Pass |
| Native REST E2E suite (11 tests) | Pass |
| C++ schedule/controller tests | Pass |
| Python simulator tests | Pass |
| Dashboard served by native firmware | Pass |

ESP32 flash size after the Zephyr port: about 759 KB flash, 183 KB DRAM,
72 KB IRAM. DRAM headroom is intentionally kept by using a 48 KB system heap.
