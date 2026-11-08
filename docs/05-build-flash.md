# 05 - Build, flash and first boot

> The production firmware now targets Zephyr RTOS. The Zephyr build and flash
> commands are in [../okf/testing.md](../okf/testing.md) and the architecture is
> in [../okf/zephyr-port.md](../okf/zephyr-port.md). The Arduino instructions
> below remain valid for the reference implementation.

## Arduino toolchain

Tested with:

- Arduino-ESP32 core **3.3.12**
- ArduinoJson **7.4.3**
- `arduino-cli` installed under `/opt/mraf-tools`

Run:

```bash
cd /workspace/mraf
./scripts/setup_toolchain.sh
```

The script:

1. Downloads `arduino-cli` into `/opt/mraf-tools/bin` if missing.
2. Creates a tool-local `arduino-cli.yaml`.
3. Stores cores and libraries under `/opt/mraf-tools`.
4. Installs the ESP32 core and ArduinoJson.

Override the location with:

```bash
MRAF_TOOLS="$HOME/mraf-tools" ./scripts/setup_toolchain.sh
```

If a large core download fails with `unexpected EOF`, rerun the script. The
script retries the core install. You can also pre-download the failed archive
into `/opt/mraf-tools/staging/packages/`; `arduino-cli` reuses files whose size
and checksum match.

## Compile

```bash
make compile
```

This runs `python3 tools/gen_webui.py` first, then:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 firmware/mraf --build-path /tmp/mraf-build
```

Expected result:

```text
Sketch uses 1121494 bytes (85%) of program storage space.
Global variables use 58356 bytes (17%) of dynamic memory.
```

Build products are in `/tmp/mraf-build`. The main binaries are
`mraf.ino.bin` and `mraf.ino.elf`.

## Upload

Connect the ESP32 over USB, then:

```bash
make flash PORT=/dev/ttyUSB0
make monitor PORT=/dev/ttyUSB0
```

On Windows with WSL, the port is usually `/dev/ttyUSB0` or `/dev/ttyACM0` only
if USB passthrough is configured. Otherwise flash from Windows with the same
`arduino-cli` command or use the Arduino IDE.

If upload fails:

- Hold the BOOT button, tap EN/RST, release BOOT, then retry.
- Try a lower upload speed: add `--upload-field speed=115200` or use the
  Arduino IDE.
- Check that the USB cable is a data cable, not charge-only.
- Install the CP210x or CH340 driver on the host.

## First boot

Open the serial monitor at **115200 baud**. You should see:

```text
RTC time: ...
ACS712 zero: ...
Wi-Fi connected: 192.168.x.x
mDNS: http://mraf.local
HTTP server started on port 80
```

If no Wi-Fi credentials are stored, the ESP32 starts an access point:

- SSID: `MRAF-xxxxxx`
- Password: `farm1234`
- URL: `http://192.168.4.1`

Log in with `admin` / `farm1234`, open **Settings**, scan for Wi-Fi, enter the
password, and save. The controller reboots and joins the network.

Once on the network, use:

```text
http://mraf.local
http://<esp32-ip>/
```

Change the dashboard password in Settings immediately. Changing auth or Wi-Fi
settings triggers an automatic reboot.

## Factory reset

Configuration is stored in NVS under the `mraf` namespace. To erase everything,
including Wi-Fi credentials and the auth password, erase the whole flash:

```bash
python3 -m esptool --chip esp32 --port /dev/ttyUSB0 erase_flash
make flash PORT=/dev/ttyUSB0
```

`esptool` is installed inside the ESP32 core tools directory. If the module is
not on your Python path, run the copy under
`/opt/mraf-tools/arduino15/packages/esp32/tools/esptool_py/`.

The controller then boots with:

- SSID empty → AP fallback
- auth `admin` / `farm1234`
- default schedules
- timezone `IST-5:30`
- pump resume enabled

## Board options

The default FQBN `esp32:esp32:esp32` targets a generic ESP32 DevKit with the
default 4 MB flash layout. If your board has different flash or PSRAM, list the
options:

```bash
arduino-cli board details -b esp32:esp32:esp32
```

Common changes:

- `PartitionScheme=huge_app` for more application space (no OTA).
- `FlashFreq=80`, `FlashMode=qio` for most DevKit V1 boards.
- A different upload speed if your cable/board is unreliable.

## Pre-power checks

Before connecting the pump:

1. Inspect for shorts between 230 V L/N/E and between 5 V and 3.3 V.
2. Confirm the relay module's JD-VCC jumper is removed.
3. Confirm the ESP32 is powered from the buck, not from the 5 V relay SMPS.
4. Confirm all relay COM/NO wiring is correct and covered.
5. Confirm the contactor coil voltage matches the control supply.
6. Confirm the safety chain is continuous and opens on E-stop, overload, phase
   failure, tank-full and sump-dry.

Then follow the commissioning checklist in
[`docs/03-wiring.md`](03-wiring.md#commissioning-checklist).
