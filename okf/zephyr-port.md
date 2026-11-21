---
okf: 1
title: Zephyr port architecture
type: architecture
updated: 2026-11-15
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
| `src/api.cpp` | Zephyr HTTP server resources, Basic auth, REST handlers (`/api/chat`, `/mcp`) |
| `src/intent.cpp` | Deterministic grammar parser and control application (`/api/chat`) |
| `src/mcp.cpp` | On-device MCP JSON-RPC 2.0 endpoint and tool registry |
| `src/remote.cpp` | CoAP polling thread, request acknowledgement, WireGuard and remote console |
| `src/console.cpp` | Remote console command runner (Zephyr-style shell) used by `remote.cpp` |
| `src/coap_client.cpp` | Confirmable CoAP POST client over UDP |
| `src/platform_native.cpp` | Simulated sensors and actuators, file settings, host networking |
| `src/platform_esp32.cpp` | GPIO, ADC, DS3231, Wi-Fi AP+STA, SNTP, mDNS, NVS settings, reboot, WireGuard |

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

## QEMU emulation

The real ESP32 firmware can run under Espressif's QEMU fork
(`/opt/qemu-xtensa-softmmu/bin/qemu-system-xtensa`, machine `esp32`):

```bash
cd /opt/zephyrproject
source /opt/zephyr-venv/bin/activate
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk-1.0.1
west build -s firmware/zephyr -b esp32_devkitc/esp32/procpu \
  -d firmware/zephyr/build/esp32-sb --sysbuild -- \
  -DCONFIG_ESPRESSIF_QEMU=y -DEXTRA_CONF_FILE=qemu.conf
```

Requirements and limitations:

- MCUboot sysbuild is mandatory; the simple-boot image does not parse under
  QEMU (`Error parsing segments`).
- The build produces `zephyr/flash_image.bin` (MCUboot@0x1000 + signed app
  @0x20000) and `zephyr/qemu_efuse_eco3.bin`. QEMU defaults to chip rev 0,
  which Zephyr rejects; the ECO3 efuse image makes the guest report v3.0.
- `qemu.conf` disables Wi-Fi (`CONFIG_WIFI=n`). QEMU does not emulate the
  radio, and the Wi-Fi driver asserts in `esp_phy_enable()` during its own
  `ppTask` boot thread, so the app guards that code on `CONFIG_WIFI_ESP32`.
- The Xtensa boot path never enables the FPU coprocessor; floats in any
  thread crash with EXCCAUSE 32. `platform_esp32.cpp` enables coprocessor 0
  via `SYS_INIT(mraf_enable_fpu, PRE_KERNEL_1, 0)`.
- No guest-reachable networking exists on the ESP32 machine (no RF emulation,
  no OpenCores Ethernet driver in this Zephyr tree), so the HTTP dashboard is
  not reachable under ESP32 QEMU; the device boots into AP-mode UI state. Use
  the qemu_x86 recipe below for host-reachable networking.

Manual run:

```bash
cd firmware/zephyr/build/esp32-sb/zephyr/zephyr
/opt/qemu-xtensa-softmmu/bin/qemu-system-xtensa -machine esp32 -nographic \
  -drive file=flash_image.bin,if=mtd,format=raw \
  -drive file=qemu_efuse_eco3.bin,if=none,format=raw,id=efuse \
  -global driver=nvram.esp32.efuse,property=drive,value=efuse \
  -global driver=timer.esp32.timg,property=wdt_disable,value=true
```

`scripts/record_qemu_console.py` runs the image under a pty, timestamps the
serial output and renders `docs/demo/mraf-qemu-esp32-boot.mp4`.

### Host-reachable networking on qemu_x86

The ESP32 machine has no network device, so the dashboard cannot be reached
there. The same application source also builds for Zephyr's `qemu_x86` board,
where the platform backend is selected by `CONFIG_MRAF_PLATFORM_NATIVE` and
QEMU's emulated `intel,e1000` NIC is bridged through user networking (slirp):

```bash
cd /opt/zephyrproject
source /opt/zephyr-venv/bin/activate
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk-1.0.1
west build -s /workspace/mraf/firmware/zephyr -b qemu_x86 \
  -d /workspace/mraf/firmware/zephyr/build/qemu_x86 \
  -- -DEXTRA_CONF_FILE=qemu-x86.conf
```

`qemu-x86.conf` enables `CONFIG_NET_QEMU_USER=y` (slirp; no tap device or
root needed) with `hostfwd=tcp::18083-:80`, plus `CONFIG_NET_DHCPV4=y`,
`CONFIG_REBOOT=y` and the native platform backend. The NIC model is
`e1000` (`CONFIG_ETH_NIC_MODEL="e1000"`); this Zephyr tree does not contain an
OpenCores `eth_opencores` driver, so e1000 is the networking path on qemu_x86.

Slirp answers DHCP with `10.0.2.15`, and the guest's port 80 is forwarded to
host port 18083, so the dashboard and REST API are reachable at
`http://127.0.0.1:18083/` while the firmware runs unmodified in the guest.

`scripts/qemu_x86_console.py` mirrors the serial console for manual poking,
and `scripts/qemu_x86_smoke.py` boots the target under a pty and runs the
HTTP smoke checks through the forwarded port (8/8). The smoke client uses
HTTP/1.1 keep-alive because slirp can reset a connection that the client
closes immediately after a power-state transition.

Footprint of the qemu_x86 build: 382 KiB text, 31 KiB data, 251 KiB BSS,
664,992 B of the 31 MiB RAM region used (2.05%); the board has no flash region
because the image is loaded straight into RAM.

qemu_x86 persistence notes: there is no filesystem mounted under the
application, so `mraf_storage_load/save` compile to stubs and configuration
lives in memory for the duration of a boot.

## Persistence

- Native: `CONFIG_MRAF_SETTINGS_PATH`, default `mraf-settings.json`.
- ESP32: `CONFIG_SETTINGS`, NVS backend, key `mraf/settings`.
- QEMU x86: no filesystem; storage stubs return `false`, so settings reset on
  reboot.
- Settings are written as one JSON blob so the existing parser and config
  schema remain the source of truth.

## RAM budget (ESP32-WROOM, no PSRAM)

The ESP32 target has no PSRAM, and `dram0_0_seg` (192 KiB) holds data, BSS and
the system heap. The default build leaves 36,520 B free; enabling
`CONFIG_MRAF_WEBRTC` costs ~32 KiB and links with 4,328 B free
(`_end 0x3ffdef18`). That margin is only possible because the firmware sizes
its static buffers to the real payloads:

- HTTP response 16 KiB, request body 4 KiB, persisted settings JSON 4 KiB
- signalling HTTP buffers 2 × 5 KiB, request mailbox 4 KiB, SDP answer 1 KiB
- data-channel replies are serialized directly into the peer tx buffer
  (`mraf_peer_tx_begin`/`mraf_peer_tx_end`), avoiding a second 8 KiB buffer
- peer tx 8 KiB (status reply ≈ 5.7 KiB), peer rx 4 KiB (config save ≈ 3.2 KiB)
- `CONFIG_MBEDTLS_AES_ROM_TABLES=y` moves the AES tables to flash
- system heap 36,864 B; main, HTTP-server, remote and WebRTC stacks are 4-6 KiB

When adding a static buffer, check the map: the build fails the link if
`dram0_0_seg` overflows, and the 4.3 KiB headroom is the gate. Runtime heap
peak (DTLS handshake + ArduinoJson status/config documents) is validated on
native_sim; confirm on hardware before shipping.

## Build environment

```bash
source /opt/zephyr-venv/bin/activate
export ZEPHYR_BASE=/opt/zephyrproject/zephyr

# Native
export ZEPHYR_TOOLCHAIN_VARIANT=host
west build -s firmware/zephyr -b native_sim/native/64 -d firmware/zephyr/build/native

# ESP32 (preferred: Zephyr SDK xtensa toolchain)
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk-1.0.1
west build -s firmware/zephyr -b esp32_devkitc/esp32/procpu -d firmware/zephyr/build/esp32

# ESP32 fallback: the Arduino ESP32 cross toolchain
export PATH=/opt/mraf-tools/arduino15/packages/esp32/tools/esp-x32/2601/bin:$PATH
export ZEPHYR_TOOLCHAIN_VARIANT=cross-compile
export CROSS_COMPILE=/opt/mraf-tools/arduino15/packages/esp32/tools/esp-x32/2601/bin/xtensa-esp32-elf-
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

A local-login walkthrough of the device dashboard in the bright theme
(native_sim running the ESP32 firmware, HTTP server on the device) is at
[../docs/demo/mraf-esp32-http-demo.mp4](../docs/demo/mraf-esp32-http-demo.mp4).
It shows theme selection, live pump and relay control, daily and recurring
schedules, settings and events. Re-record it with
`scripts/record_esp32_demo.js` and the same Playwright setup described in
`docs/11-springboot-server.md`.

A focused walkthrough of pointing the device at the remote CoAP server and
bringing up the WireGuard tunnel is at
[../docs/demo/mraf-remote-tunnel-demo.mp4](../docs/demo/mraf-remote-tunnel-demo.mp4).
Re-record it with `scripts/record_remote_tunnel_demo.js`.

A 35-second terminal walkthrough of the native build, ESP32 build, E2E suite
and live API is at
[../docs/demo/zephyr-native-demo.mp4](../docs/demo/zephyr-native-demo.mp4).

A recording of the actual ESP32 flash image booting under Espressif QEMU
(ROM → MCUboot → Zephyr → `MRAF 1.1.0 on esp32, HTTP port 80`) is at
[../docs/demo/mraf-qemu-esp32-boot.mp4](../docs/demo/mraf-qemu-esp32-boot.mp4).
Re-record it with `scripts/record_qemu_console.py`.

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
