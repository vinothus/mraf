# 14 - Flash the Zephyr firmware from an Android phone (USB OTG)

This guide burns `firmware/zephyr/build/esp32/zephyr/zephyr.bin` directly from an
Android phone, without a PC. It works with any flasher that implements the
Espressif ROM download protocol (SLIP framing) over USB OTG. The reference app
is **ESP32_Flasher** (`com.esp_flash.esp_flash_app`, Eric Parisot) because it
supports ESP32-WROOM-32, per-file flash offsets, whole-chip erase and a serial
monitor. The values below also apply to your own app built on the same
ROM/SLIP SDK — only the menu labels change.

> This is USB flashing only. The Zephyr firmware currently has no BLE
> provisioning and no OTA service; see [13-webrtc.md](13-webrtc.md) and
> [../okf/zephyr-port.md](../okf/zephyr-port.md) for what runs on the device.

## 1. Get the image

Build the default controller:

```bash
cd /workspace/mraf
make zephyr-build-esp32
# or: scripts/zephyr_build.sh esp32
```

Build the WebRTC overlay instead (same flash procedure):

```bash
cd /opt/zephyrproject
source /opt/zephyr-venv/bin/activate
export ZEPHYR_BASE=/opt/zephyrproject/zephyr
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
west build -p always -s /workspace/mraf/firmware/zephyr \
  -b esp32_devkitc/esp32/procpu -d /workspace/mraf/firmware/zephyr/build/esp32-webrtc \
  -- -DEXTRA_CONF_FILE=webrtc.conf
```

| Build | File to copy to the phone | Offset | Size (this workspace) |
|---|---|---|---|
| Default | `firmware/zephyr/build/esp32/zephyr/zephyr.bin` | `0x1000` | 913,408 B |
| WebRTC | `firmware/zephyr/build/esp32-webrtc/zephyr/zephyr.bin` | `0x1000` | 978,944 B |

Sizes change with every build; trust the file you actually copy, not the table.
Never flash `zephyr.elf` and never split the image.

Why one file at `0x1000`: Zephyr's ESP32 build uses `CONFIG_ESP_SIMPLE_BOOT`, so
it produces a single ESP application image whose header sits at `0x1000`
(the same address `west flash --runner esp32` writes). There is no ESP-IDF
bootloader/partition-table/app triple to flash at `0x0/0x8000/0x10000`. The
image header already encodes DIO, 40 MHz and 4 MB flash.

Record the checksum so you can detect a corrupted copy:

```bash
sha256sum firmware/zephyr/build/esp32/zephyr/zephyr.bin
```

The first byte of a valid image is `0xE9` (the ESP image magic).

## 2. What you need

- Android phone with USB host/OTG support (most modern phones).
- USB-OTG adapter or the correct cable for the phone.
- ESP32-WROOM-32 DevKit board (CP2102 or CH340 USB-UART bridge; native-USB
  ESP32-S3 boards also work, but this firmware targets the classic ESP32).
- **Data** USB cable (not charge-only).
- The build above copied to the phone (e.g. `Download/zephyr.bin`).
- The board **not** connected to mains, relays or the pump while flashing.

Safety first: disconnect 230 V and all loads before touching the board. Flash
with the controller on USB only.

## 3. Prepare the phone

1. Install ESP32_Flasher from Google Play
   (`https://play.google.com/store/apps/details?id=com.esp_flash.esp_flash_app`).
2. On some phones (Xiaomi/Redmi, Realme, OnePlus) enable **Settings → OTG** or
   **USB → OTG connection**; otherwise the app sees no device.
3. Keep the screen awake and the phone above ~40% battery.
4. Do not open another app that claims the USB serial port while flashing.

## 4. Flash

1. Connect the DevKit to the phone with the OTG adapter. Android asks for USB
   permission — allow it.
2. Open ESP32_Flasher and pick the device: **ESP32 / ESP32-WROOM-32**.
3. Put the board into download mode manually (most reliable):
   - hold **BOOT** (GPIO0),
   - tap **EN/RST**,
   - release **BOOT**.
   If the app's automatic bootloader entry works for your board you can skip
   this; disable “auto bootloader” if it fails.
4. Add the firmware file and set its address to `0x1000`.
   - Treat any address other than `0x1000` as wrong for this firmware.
5. Match the flash options if the app asks (its defaults are usually fine):
   - Flash size: **4 MB**
   - SPI mode: **DIO**
   - SPI frequency: **40 MHz**
   - Baud: start at **115200** for the first flash; 460800/921600 once it works
   - Compression: on; if flashing stalls, disable compression and retry
6. Optional but recommended for a clean install / recovered board: **Erase
   flash** first. This wipes NVS, so Wi-Fi credentials, the dashboard password
   and schedules return to defaults (see §6).
7. Press **Flash**. Do not unplug, background the app, or let the phone sleep
   during the write.
8. Wait for a successful report (address written, hash verified, hard reset).
   The board reboots automatically; if not, press **EN/RST**.

Equivalent desktop command, for reference:

```bash
python3 -m esptool --chip esp32 --port /dev/ttyUSB0 \
  write_flash --flash_mode dio --flash_freq 40m --flash_size 4MB \
  0x1000 firmware/zephyr/build/esp32/zephyr/zephyr.bin
```

## 5. Verify the boot

Open the app's serial monitor (or any 115200-baud terminal) and press **EN/RST**.
A healthy default build prints:

```text
*** Booting Zephyr OS build v4.5.0 ***
MRAF 1.1.0 on esp32, HTTP port 80, device SIM-XXXXXXXX
```

Then check the HTTP API once the board is on Wi-Fi:

```bash
curl -u admin:farm1234 http://<esp32-ip>/health
curl -u admin:farm1234 http://<esp32-ip>/api/status
```

The WebRTC overlay prints the same banner; its extra lines appear only when a
browser connects (`webrtc: peer listening ...`, `DTLS handshake complete`).

## 6. First boot and Wi-Fi

If no Wi-Fi credentials are stored (fresh board, or after an erase), the
controller starts its own access point:

- SSID: `MRAF-AP`
- Password: `farm1234` (or the configured dashboard password)
- URL: `http://192.168.4.1`

Log in with `admin` / `farm1234`, open **Settings**, scan for Wi-Fi, enter the
password and save. The controller reboots and joins the network. Then use:

```text
http://mraf.local
http://<esp32-ip>/
```

Change the dashboard password in Settings immediately. Changing Wi-Fi or auth
settings triggers an automatic reboot.

For the WebRTC overlay, also set the signalling server and token in Settings
(`webrtc.enabled`, `webrtc.server`, optional `webrtc.stun`,
`webrtc.poll_ms 100..5000`) and `coap.token` to the server's demo token.

## 7. Factory reset from the phone

Use the app's **Erase flash** action, then flash the image again. Erasing the
whole chip removes the NVS `mraf` namespace, so the board boots with:

- SSID empty → `MRAF-AP` fallback, `192.168.4.1`
- auth `admin` / `farm1234`
- default schedules
- timezone `IST-5:30`
- pump resume enabled

## 8. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| App finds no device | OTG disabled in phone settings, charge-only cable, or another app owns the port. |
| “Failed to connect” / sync error | Board not in download mode — hold BOOT, tap EN, release BOOT, retry. |
| Stalls mid-write or times out | Lower baud to 115200, disable compression, use a short data cable, keep the screen on. |
| Boot loop, `invalid header: 0xffffffff` | Wrong offset or wrong file. Offset must be `0x1000` and the file must be `zephyr.bin`, not the ELF. Erase and reflash. |
| `invalid header` after a good write | Copy corrupted (phone transfer); re-copy, compare size and SHA-256. |
| Boots but no Wi-Fi | Erase wiped NVS — join `MRAF-AP` and reconfigure, or flash without erasing. |
| Phone reboots/drops USB | OTG power brown-out; use a short cable, a powered OTG hub, or a charged phone. |
| Image too large | You flashed the wrong build or an ESP-IDF-style merged file; use the single Zephyr `zephyr.bin`. |

## 9. Scope and limitations

- Only USB-OTG ROM/SLIP flashing is covered. There is no BLE provisioning or
  in-field OTA in the current firmware; adding OTA needs MCUboot image slots and
  an update endpoint on the server.
- Flashing is unauthenticated (no secure boot), so anyone with USB access can
  rewrite the board. Physical access is the trust boundary until secure boot is
  enabled.
- The app is a generic ESP32 flasher, not MRAF-specific. The only
  MRAF-specific values are the file, the `0x1000` offset and the 4 MB / DIO /
  40 MHz flash geometry.
