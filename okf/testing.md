---
okf: 1
title: Build and test guide
type: testing
updated: 2026-11-15
tags: [build, test, e2e, zephyr, simulator, verification, qemu]
---

# Build and test guide

## Prerequisites

Host install steps for Linux and Windows: [../docs/15-toolchain-setup.md](../docs/15-toolchain-setup.md).

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
- chat grammar (`/api/chat`), WireGuard status (`/api/wireguard`) and the MCP
  endpoint (`/mcp`, tools/list, tools/call, 202 notification)

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

## Emulate ESP32 under QEMU

The real firmware flash image runs under Espressif's QEMU fork (see
`zephyr-port.md` for the recipe). Required: MCUboot sysbuild with
`CONFIG_ESPRESSIF_QEMU=y` and `-DEXTRA_CONF_FILE=qemu.conf` (Wi-Fi off),
because QEMU does not emulate the radio and the driver would assert. Boot
check:

```bash
cd firmware/zephyr/build/esp32-sb/zephyr/zephyr
timeout 30 /opt/qemu-xtensa-softmmu/bin/qemu-system-xtensa -machine esp32 \
  -nographic -drive file=flash_image.bin,if=mtd,format=raw \
  -drive file=qemu_efuse_eco3.bin,if=none,format=raw,id=efuse \
  -global driver=nvram.esp32.efuse,property=drive,value=efuse \
  -global driver=timer.esp32.timg,property=wdt_disable,value=true
```

Pass criteria: ROM boot → MCUboot loads slot 0 → `*** Booting Zephyr OS
build v4.5.0 ***` → `MRAF 1.1.0 on esp32, HTTP port 80, device ESP32-000000`
and the main loop stays alive. Recorded demo:
`docs/demo/mraf-qemu-esp32-boot.mp4`.

## Emulate the x86 target with networking

`qemu_x86` uses QEMU user networking (slirp) and forwards guest port 80 to
host 18083, so the dashboard is reachable while the firmware runs in the
emulator. No tap device or root privileges are required.

```bash
cd /opt/zephyrproject
source /opt/zephyr-venv/bin/activate
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk-1.0.1
west build -s /workspace/mraf/firmware/zephyr -b qemu_x86 \
  -d /workspace/mraf/firmware/zephyr/build/qemu_x86 \
  -- -DEXTRA_CONF_FILE=qemu-x86.conf

# Boot under a pty and run the HTTP smoke checks through the forwarded port
python3 /workspace/mraf/scripts/qemu_x86_smoke.py
```

Pass criteria: banner `MRAF 1.1.0 on qemu_x86, HTTP port 80, device SIM-…`
and 8/8 PASS (health, dashboard, authenticated status with `ip=10.0.2.15`,
config GET/POST, sim power off, sim power on, status after POST). The guest
gets `10.0.2.15` over DHCP; the host reaches it at `http://127.0.0.1:18083/`.
Keep the HTTP client on keep-alive: slirp can reset a connection that is
closed immediately after a power-state transition. `scripts/qemu_x86_console.py`
mirrors the serial console for manual API poking.

qemu_x86 build footprint: 382 KiB text, 31 KiB data, 251 KiB BSS, 664,992 B
of the 31 MiB RAM region used (2.05%). The board has no flash region (the
image loads directly into RAM), and there is no filesystem, so settings
persistence is compiled out and configuration resets on reboot.

## Verification record

| Check | Result |
|---|---|
| Zephyr native_sim build | Pass |
| Zephyr ESP32 build with Espressif blobs | Pass |
| Native REST E2E suite (15 tests) | Pass |
| C++ schedule/controller tests | Pass |
| Python simulator tests (38 tests) | Pass |
| Dashboard served by native firmware | Pass |
| ESP32 flash image boots under Espressif QEMU (MCUboot sysbuild, Wi-Fi off) | Pass |
| qemu_x86 build (e1000 + slirp, hostfwd 18083→80) | Pass — 664,992 B RAM (2.05%), 382 KiB text |
| qemu_x86 HTTP smoke through forwarded port | Pass — 8/8 |
| Chrome E2E over the WebRTC data channel (status/control/chat/transport) | Pass |
| ESP32 + `CONFIG_MRAF_WEBRTC=y` link (`webrtc.conf`) | Pass — 4,328 B free in `dram0_0_seg` |

ESP32 flash size after the Zephyr port: about 838 KiB flash, 70 KiB IRAM.
Default DRAM is 160,088 B of the 192 KiB region (81.4%); with the WebRTC
overlay it is 192,280 B (97.8%, `_end 0x3ffdef18`). The fit relies on the
trimmed buffers and the 36 KiB system heap documented in
[`docs/13-webrtc.md`](../docs/13-webrtc.md); re-check `dram0_0_seg` after any
new static buffer.

Chrome E2E (`/tmp/opencode/chrome_webrtc.py` against the native_sim firmware
and a local signalling server) reports `DC: connected`, a 12-relay status
payload, relay control, a chat reply and `ice: connected`. The native REST
suite still passes 15/15 and qemu_x86 smoke 8/8 with the trimmed buffers.
