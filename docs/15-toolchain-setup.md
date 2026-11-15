# 15 - Toolchain setup (Windows and Linux)

This document installs everything needed to build, run and flash the MRAF
firmware on a developer machine. Linux and Windows are covered separately
because the host tools, USB access and shell commands differ.

Two firmware toolchains exist in this repository:

| Toolchain | Used for | Location |
|---|---|---|
| **Zephyr 4.5 + Zephyr SDK 1.0.1** | Production firmware (`firmware/zephyr`) for ESP32 and `native_sim` | This document |
| **Arduino-ESP32 3.3.12** | Reference firmware (`firmware/mraf`) | This document, last section |
| Android phone only | Flashing without a PC | [14-android-flash.md](14-android-flash.md) |

## 1. Support matrix

| Host | Zephyr ESP32 build | `native_sim` | REST E2E / QEMU smoke | USB flashing |
|---|---|---|---|---|
| Linux x86_64 (Ubuntu 24.04) | Verified | Verified | Verified | `esptool` via `/dev/ttyUSB0` |
| Windows 11 + WSL2 (Ubuntu 24.04) | Verified (same Linux steps) | Verified | Verified | `usbipd-win`, or `esptool` from PowerShell on `COMx` |
| Windows 11 native | Documented, builds with Zephyr SDK | Not supported by this project | Not supported | `esptool` on `COMx` |

Use **WSL2 on Windows** unless you only need to compile/flash the ESP32. The
project's `Makefile` and `scripts/*.sh` are Bash scripts, and the `native_sim`
build in this repository relies on POSIX offloaded sockets.

## 2. Versions used by this project

Pin these versions when reproducing a build or reporting a bug.

| Component | Version | Notes |
|---|---|---|
| Zephyr | v4.5.0 | Checked out under `zephyrproject/zephyr` |
| west | 1.5.0 | Installed with `pip` inside the venv |
| Python | 3.12.x | 3.10+ works; 3.12 is what CI uses |
| CMake | >= 3.28 | Zephyr 4.5 minimum |
| Ninja | any recent | Build backend |
| Devicetree compiler (`dtc`) | >= 1.4.6 | Host package |
| Zephyr SDK | 1.0.1 | Provides GCC 14.3 for Xtensa and x86, plus QEMU host tools |
| esptool | 5.3.1 | Flashing ESP32 over serial |
| Arduino-ESP32 core | 3.3.12 | Reference firmware only |
| ArduinoJson | 7.4.3 | Reference firmware only |
| Node.js | 18+ | Optional: Matter bridge, dashboard tooling |
| JDK 17 + Maven | 3.9+ | Optional: Spring Boot server |

Disk and network: a Zephyr workspace plus the full SDK needs roughly 15 GB.
`west update`, `west sdk install` and `west blobs fetch` download from GitHub.

## 3. Linux setup

The commands below target Ubuntu 24.04 / Debian 12 and also work inside WSL2.
They use the same layout as this workspace (`/opt/zephyrproject`,
`/opt/zephyr-venv`, `/opt/zephyr-sdk-1.0.1`). If you install under your home
directory instead, see section 5 for the path overrides.

### 3.1 Host packages

```bash
sudo apt update
sudo apt install -y \
  git cmake ninja-build gperf device-tree-compiler \
  python3 python3-dev python3-venv python3-pip python3-tk \
  xz-utils file make gcc gcc-multilib g++-multilib \
  libsdl2-dev libmagic1 wget curl usbutils
```

On AArch64 omit `gcc-multilib g++-multilib`. `gperf`, `dtc` and the Python
development headers are required by Zephyr's CMake and devicetree steps.

### 3.2 Python, west and the Zephyr workspace

```bash
python3 -m venv /opt/zephyr-venv
source /opt/zephyr-venv/bin/activate
pip install --upgrade pip
pip install west

west init -m https://github.com/zephyrproject-rtos/zephyr --mr v4.5.0 /opt/zephyrproject
cd /opt/zephyrproject
west update
west packages pip --install
west zephyr-export
pip install esptool==5.3.1
```

`west packages pip --install` installs the Python requirements belonging to
the checked-out Zephyr tree, including `pyelftools` and `pyserial`. The last
line adds the flasher used by `west flash`.

If `west sdk` later reports `No module named 'patoolib'`:

```bash
pip install patool
```

For a per-user install, replace `/opt/zephyr-venv` and `/opt/zephyrproject`
with e.g. `$HOME/zephyrproject/.venv` and `$HOME/zephyrproject`. Create the
parent directory first (`mkdir -p ~/zephyrproject`).

### 3.3 Zephyr SDK 1.0.1

The SDK contains the Xtensa compiler for the ESP32, the x86 compiler for QEMU,
and host tools such as QEMU and OpenOCD.

```bash
source /opt/zephyr-venv/bin/activate
cd /opt/zephyrproject/zephyr
west sdk install --version 1.0.1 --install-base /opt
```

This installs to `/opt/zephyr-sdk-1.0.1` and registers the SDK CMake package.
If `/opt` is not writable by your user, either prefix the command with `sudo`
(and keep the venv active) or use `--install-base "$HOME"` and set
`ZEPHYR_SDK_INSTALL_DIR` accordingly. To save disk space you can pass `-t` with
one or more GNU toolchains, for example
`-t xtensa-espressif_esp32_zephyr-elf x86_64-zephyr-elf`; omit `-t` to install
all of them. QEMU host tools are included either way.

Manual alternative (offline installs or older west):

```bash
cd /opt
wget https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v1.0.1/zephyr-sdk-1.0.1_linux-x86_64.tar.xz
tar xf zephyr-sdk-1.0.1_linux-x86_64.tar.xz
cd zephyr-sdk-1.0.1
./setup.sh
```

`setup.sh` needs CMake on `PATH`. It registers the SDK in the CMake user
package registry, so `ZEPHYR_SDK_INSTALL_DIR` is not strictly required, but
this repository's scripts default to `/opt/zephyr-sdk-1.0.1`, so export it as
shown in section 5.

### 3.4 Espressif HAL blobs

The ESP32 Wi-Fi and PHY code ships as binaries. Fetch them once after
`west update`, and again after switching Zephyr versions:

```bash
cd /opt/zephyrproject
source /opt/zephyr-venv/bin/activate
west blobs fetch hal_espressif
```

Skipping this fails the ESP32 link step with missing `hal_espressif` symbols.

### 3.5 Build and test

With `ZEPHYR_BASE` exported, the repository targets work from any directory:

```bash
cd /workspace/mraf
make zephyr-build-native     # native_sim, dashboard on http://127.0.0.1:8080
make zephyr-e2e              # 15 REST end-to-end tests against native_sim
make zephyr-build-esp32      # firmware/zephyr/build/esp32/zephyr/zephyr.bin
```

WebRTC overlay:

```bash
cd /opt/zephyrproject
source /opt/zephyr-venv/bin/activate
export ZEPHYR_BASE=/opt/zephyrproject/zephyr
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
west build -p always -s /workspace/mraf/firmware/zephyr \
  -b esp32_devkitc/esp32/procpu \
  -d /workspace/mraf/firmware/zephyr/build/esp32-webrtc \
  -- -DEXTRA_CONF_FILE=webrtc.conf
```

Run the native binary directly:

```bash
./firmware/zephyr/build/native/zephyr/zephyr.exe
```

QEMU x86 smoke test (uses QEMU from the Zephyr SDK host tools):

```bash
export ZEPHYR_BASE=/opt/zephyrproject/zephyr
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk-1.0.1
west build -p always -s firmware/zephyr -b qemu_x86 \
  -d firmware/zephyr/build/qemu_x86 -- -DEXTRA_CONF_FILE=qemu-x86.conf
python3 scripts/qemu_x86_smoke.py
```

### 3.6 Serial access and flashing

Add your user to the serial group and reconnect (log out/in, or run
`newgrp dialout`):

```bash
sudo usermod -aG dialout "$USER"
```

For boards that appear without permissions, install udev rules for the common
USB-UART bridges:

```bash
sudo tee /etc/udev/rules.d/99-mraf-esp32.rules >/dev/null <<'EOF'
SUBSYSTEM=="tty", ATTRS{idVendor}=="1a86", ATTRS{idProduct}=="7523", MODE="0666", GROUP="dialout"
SUBSYSTEM=="tty", ATTRS{idVendor}=="10c4", ATTRS{idProduct}=="ea60", MODE="0666", GROUP="dialout"
EOF
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Flashing and monitoring:

```bash
make zephyr-flash PORT=/dev/ttyUSB0
make zephyr-monitor PORT=/dev/ttyUSB0
```

The device usually shows up as `/dev/ttyUSB0` (CH340/CP210x) or
`/dev/ttyACM0` (native USB). If `west flash` cannot find the port, run
`ls -l /dev/ttyUSB* /dev/ttyACM*` and `dmesg | tail`.

### 3.7 Arduino reference firmware (optional)

```bash
cd /workspace/mraf
export MRAF_TOOLS=/opt/mraf-tools
./scripts/setup_toolchain.sh
export PATH="$MRAF_TOOLS/bin:$PATH"
export ARDUINO_CONFIG_FILE="$MRAF_TOOLS/arduino-cli.yaml"
make compile
make flash PORT=/dev/ttyUSB0
make monitor PORT=/dev/ttyUSB0
```

`setup_toolchain.sh` installs `arduino-cli`, the ESP32 core and ArduinoJson
under `$MRAF_TOOLS`. Pin the tested core explicitly if you need reproducibility:

```bash
arduino-cli --config-file "$ARDUINO_CONFIG_FILE" core install esp32:esp32@3.3.12
```

`make compile` reads `MRAF_TOOLS`/`ARDUINO_CONFIG_FILE`, but the `flash` and
`monitor` targets use the `TOOLS_DIR` make variable (default `/opt/mraf-tools`).
When you installed under `$HOME`, pass it to those targets as well:

```bash
make flash PORT=/dev/ttyUSB0 TOOLS_DIR="$MRAF_TOOLS"
make monitor PORT=/dev/ttyUSB0 TOOLS_DIR="$MRAF_TOOLS"
```

### 3.8 Optional extras

```bash
# Spring Boot server (JDK 17 + Maven)
sudo apt install -y openjdk-17-jdk maven

# Matter bridge (Node 18+)
sudo apt install -y nodejs npm

# Dashboard/WebRTC recording and browser E2E (Python Playwright)
pip install playwright
python3 -m playwright install chromium
```

## 4. Windows setup

### 4.1 Option A: WSL2 (recommended)

WSL2 gives you the exact Linux toolchain above, including `native_sim`, the
Bash scripts, E2E tests and QEMU.

```powershell
wsl --install -d Ubuntu-24.04
wsl --update
```

Open Ubuntu, then follow **section 3** inside WSL. Keep the repository in the
WSL filesystem (e.g. `~/mraf`), not under `/mnt/c/...`: builds on the Windows
filesystem are much slower and can hit permission/path issues.

#### USB serial passthrough with usbipd-win

Windows owns the COM port until you attach the USB device to WSL.

```powershell
winget install usbipd
```

Plug in the ESP32, then in an **elevated** PowerShell:

```powershell
usbipd list
usbipd bind --busid <BUSID>        # once per device
usbipd attach --wsl --busid <BUSID>
```

In WSL:

```bash
ls -l /dev/ttyUSB*
sudo usermod -aG dialout "$USER"
```

Re-run `usbipd attach --wsl --busid <BUSID>` after unplugging, resetting, or
rebooting the board. To return the device to Windows:
`usbipd detach --busid <BUSID>`.

If the board does not appear as `/dev/ttyUSB*`, check `usbipd list` for state
`Attached`, run `dmesg | tail`, and confirm the CP210x/CH340 driver is
installed on Windows.

#### Alternative: build in WSL, flash from Windows

If USB passthrough is unavailable, run `esptool` on the Windows side against
the COM port. The binary is readable through the WSL share:

```powershell
py -3.12 -m pip install esptool==5.3.1
py -3.12 -m esptool --chip esp32 -p COM5 -b 460800 erase_flash
py -3.12 -m esptool --chip esp32 -p COM5 -b 460800 write_flash `
  --flash_mode dio --flash_freq 40m --flash_size 4MB `
  0x1000 "\\wsl.localhost\Ubuntu-24.04\home\<user>\mraf\firmware\zephyr\build\esp32\zephyr\zephyr.bin"
```

Use the `esp32-webrtc` path instead for the WebRTC build. See
[14-android-flash.md](14-android-flash.md) for why the offset is `0x1000` and
why only one file is written.

### 4.2 Option B: native Windows (ESP32 only)

This path builds and flashes the ESP32 firmware with the Zephyr SDK. Do **not**
use it for `native_sim`, REST E2E or QEMU smoke tests; use WSL2 for those.

#### Host tools

```powershell
winget install Kitware.CMake Ninja-build.Ninja oss-winget.gperf `
  Python.Python.3.12 Git.Git oss-winget.dtc wget 7zip.7zip
```

Close and reopen the terminal so `PATH` is refreshed, then verify:

```powershell
cmake --version
ninja --version
dtc --version
py -3.12 --version
```

Install the USB-UART driver for your board (Silicon Labs CP210x or WCH CH340)
and note the port in Device Manager, e.g. `COM5`.

#### Python, west and the Zephyr workspace

```powershell
cd $Env:USERPROFILE
py -3.12 -m venv zephyrproject\.venv
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser
zephyrproject\.venv\Scripts\Activate.ps1

pip install --upgrade pip
pip install west

west init -m https://github.com/zephyrproject-rtos/zephyr --mr v4.5.0 zephyrproject
cd zephyrproject
west update
cmd /c zephyr\scripts\utils\west-packages-pip-install.cmd
west zephyr-export
pip install esptool==5.3.1
```

#### Zephyr SDK 1.0.1

```powershell
cd $Env:USERPROFILE\zephyrproject\zephyr
west sdk install --version 1.0.1 --install-base $Env:USERPROFILE
```

This installs to `%USERPROFILE%\zephyr-sdk-1.0.1` and runs the SDK setup for you.
If you use the standalone Windows installer from the SDK releases instead, make
sure its `setup.cmd` completed successfully.

#### Espressif HAL blobs

```powershell
cd $Env:USERPROFILE\zephyrproject
zephyrproject\.venv\Scripts\Activate.ps1
west blobs fetch hal_espressif
```

#### Build from PowerShell

Run from the repository root (`cd C:\path\to\mraf`), with the venv active:

```powershell
$env:ZEPHYR_BASE = "$Env:USERPROFILE\zephyrproject\zephyr"
$env:ZEPHYR_SDK_INSTALL_DIR = "$Env:USERPROFILE\zephyr-sdk-1.0.1"
$env:ZEPHYR_TOOLCHAIN_VARIANT = "zephyr"

west build -p always -s firmware/zephyr -b esp32_devkitc/esp32/procpu `
  -d firmware\zephyr\build\esp32

west build -p always -s firmware/zephyr -b esp32_devkitc/esp32/procpu `
  -d firmware\zephyr\build\esp32-webrtc -- -DEXTRA_CONF_FILE=webrtc.conf
```

Outputs:

- `firmware\zephyr\build\esp32\zephyr\zephyr.bin`
- `firmware\zephyr\build\esp32-webrtc\zephyr\zephyr.bin`

#### Flash and monitor from PowerShell

With esptool installed in the active venv:

```powershell
west flash -d firmware\zephyr\build\esp32 --esp-device COM5

# esptool equivalent
python -m esptool --chip esp32 -p COM5 -b 460800 erase_flash
python -m esptool --chip esp32 -p COM5 -b 460800 write_flash `
  --flash_mode dio --flash_freq 40m --flash_size 4MB `
  0x1000 firmware\zephyr\build\esp32\zephyr\zephyr.bin
```

Monitor at 115200 baud:

```powershell
python -m serial.tools.miniterm COM5 115200
```

Close the monitor before flashing again; a busy COM port is the most common
`could not open port` error.

#### Use the repository scripts from Git Bash (optional)

The Bash scripts work on Windows through Git Bash. Paths are POSIX-style:

```bash
cd /c/path/to/mraf
export ZEPHYR_BASE="$HOME/zephyrproject/zephyr"
export ZEPHYR_SDK_INSTALL_DIR="$HOME/zephyr-sdk-1.0.1"
source "$HOME/zephyrproject/.venv/Scripts/activate"
./scripts/zephyr_build.sh esp32
```

Do not point `PORT` at `COM5` for the Bash scripts; they expect a Unix device
node. Use `west flash --esp-device COM5` or `esptool` from PowerShell instead.

### 4.3 Arduino CLI on native Windows

`scripts/setup_toolchain.sh` is Bash-only. Either run it in Git Bash/WSL or
install the same components manually:

```powershell
winget install ArduinoSA.CLI
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.12
arduino-cli lib install ArduinoJson@7.4.3

# Embed web/index.html into firmware/mraf/webui.h
python tools\gen_webui.py

arduino-cli compile --fqbn esp32:esp32:esp32 --build-path C:\temp\mraf-build firmware\mraf
arduino-cli upload -p COM5 --fqbn esp32:esp32:esp32 --input-dir C:\temp\mraf-build
```

### 4.4 Windows caveats

- **native_sim is not supported** on native Windows in this project. The
  native build uses POSIX sockets and the repository scripts are Bash. Use
  WSL2.
- **Long paths**: enable Windows long path support and
  `git config --global core.longpaths true`, or clone to a short path such as
  `C:\mraf`.
- **Do not build in OneDrive-synced folders**; file locking and sync cause
  spurious build failures.
- **Antivirus**: exclude the build directory and the Zephyr workspace for
  faster builds.
- **`make` is not required** for the Zephyr flow; use `west build` directly, or
  install GNU make via MSYS2 (`pacman -S make`) and work in Git Bash.
- **Always activate the venv** before `west`, `pip` or `esptool`; otherwise
  `west` may resolve to a different Python installation.

## 5. Environment variable reference

The `Makefile` and `scripts/*.sh` default to this workspace's `/opt` layout.
For a per-user install, export the overrides below (add them to
`~/.bashrc`/`~/.mraf-env` on Linux/WSL, or set them per PowerShell session on
Windows).

| Variable | Used by | Default | Personal install |
|---|---|---|---|
| `ZEPHYR_VENV` | `scripts/zephyr_build.sh`, `make zephyr-flash` | `/opt/zephyr-venv` | `$HOME/zephyrproject/.venv` |
| `ZEPHYR_BASE` | scripts, `make`, `west` | `/opt/zephyrproject/zephyr` | `$HOME/zephyrproject/zephyr` |
| `ZEPHYR_SDK_INSTALL_DIR` | `scripts/zephyr_build.sh` | `/opt/zephyr-sdk-1.0.1` | `$HOME/zephyr-sdk-1.0.1` |
| `ZEPHYR_TOOLCHAIN_VARIANT` | set by scripts | `host` (native), `zephyr` (ESP32) | leave to scripts |
| `ESPTOOL` | `scripts/zephyr_build.sh` (adds dir to `PATH`) | Arduino `esptool_py/5.3.1` | `$HOME/zephyrproject/.venv/bin` |
| `ZEPHYR_ESPTOOL` | `make zephyr-flash` | Arduino `esptool_py/5.3.1` | `$HOME/zephyrproject/.venv/bin` |
| `MRAF_TOOLS` | `scripts/setup_toolchain.sh` | `/opt/mraf-tools` | `$HOME/mraf-tools` |
| `ARDUINO_CLI` | `scripts/setup_toolchain.sh` | `$MRAF_TOOLS/bin/arduino-cli` | same |
| `PORT` | `make flash/zephyr-flash/monitor` | `/dev/ttyUSB0` | `/dev/ttyUSB0` (Linux/WSL), `COM5` (native Windows CLI) |
| `FQBN` | Arduino `make compile/flash` | `esp32:esp32:esp32` | same |
| `BUILD_DIR` | Arduino `make compile/flash` | `/tmp/mraf-build` | any writable path |
| `PRISTINE` | `scripts/zephyr_build.sh` | `0` | `1` for a clean build |

Linux/WSL example:

```bash
export ZEPHYR_VENV="$HOME/zephyrproject/.venv"
export ZEPHYR_BASE="$HOME/zephyrproject/zephyr"
export ZEPHYR_SDK_INSTALL_DIR="$HOME/zephyr-sdk-1.0.1"
export PATH="$ZEPHYR_VENV/bin:$PATH"
```

Then:

```bash
make zephyr-build-native
make zephyr-build-esp32
make zephyr-e2e
make zephyr-flash PORT=/dev/ttyUSB0 \
  ZEPHYR_VENV="$ZEPHYR_VENV" \
  ZEPHYR_BASE="$ZEPHYR_BASE" \
  ZEPHYR_SDK_INSTALL_DIR="$ZEPHYR_SDK_INSTALL_DIR" \
  ZEPHYR_ESPTOOL="$ZEPHYR_VENV/bin"
```

To use the exact CI layout without overrides, create `/opt/zephyrproject`,
`/opt/zephyr-venv`, `/opt/zephyr-sdk-1.0.1` and `/opt/mraf-tools` on Linux and
follow section 3 unchanged.

## 6. Verify the installation

Run these checks before building firmware. Set the paths you used in section 3;
the values below are the `/opt` defaults (use your per-user paths instead if
you installed there):

```bash
export ZEPHYR_VENV=/opt/zephyr-venv
export ZEPHYR_BASE=/opt/zephyrproject/zephyr
export ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk-1.0.1
source "$ZEPHYR_VENV/bin/activate"

west --version
# West version: v1.5.0

cmake --version
# cmake version 3.28 or newer

python --version
# Python 3.12.x

"$ZEPHYR_SDK_INSTALL_DIR/gnu/xtensa-espressif_esp32_zephyr-elf/bin/xtensa-espressif_esp32_zephyr-elf-gcc" --version
# ... (Zephyr SDK 1.0.1) 14.3.0

west blobs list hal_espressif

cd /workspace/mraf
make zephyr-build-native
make zephyr-e2e
# 15/15 tests OK

make zephyr-build-esp32
ls -l firmware/zephyr/build/esp32/zephyr/zephyr.bin
```

If the Xtensa compiler is not on `PATH`, invoke it through its full path
(`$ZEPHYR_SDK_INSTALL_DIR/gnu/xtensa-espressif_esp32_zephyr-elf/bin/...`);
`west build` finds it through the SDK CMake package.

Optional hardware check with the board connected and not mounted by another
program:

```bash
python -m esptool --chip esp32 -p /dev/ttyUSB0 chip_id
```

Expected: `Chip is ESP32-D0WD-V3 (revision v3.x)` (or D0WD) and
`Detected flash size: 4MB`.

### Native Windows

Run the same version checks in the activated venv, then the ESP32 build only:

```powershell
$env:ZEPHYR_BASE = "$Env:USERPROFILE\zephyrproject\zephyr"
$env:ZEPHYR_SDK_INSTALL_DIR = "$Env:USERPROFILE\zephyr-sdk-1.0.1"
$env:ZEPHYR_TOOLCHAIN_VARIANT = "zephyr"

west --version
cmake --version
python --version
west blobs list hal_espressif

cd C:\path\to\mraf
west build -p always -s firmware/zephyr -b esp32_devkitc/esp32/procpu `
  -d firmware\zephyr\build\esp32
ls firmware\zephyr\build\esp32\zephyr\zephyr.bin
```

The REST E2E suite and `native_sim` are expected to run in WSL2, not in this
native Windows shell.

## 7. Troubleshooting

### Linux / WSL

| Symptom | Cause | Fix |
|---|---|---|
| `west: command not found` | venv not active | `source "$ZEPHYR_VENV/bin/activate"` |
| `ModuleNotFoundError: No module named 'elftools'` | Zephyr Python requirements missing | `west packages pip --install` |
| `ModuleNotFoundError: No module named 'patoolib'` from `west sdk` | `patool` not installed | `pip install patool` |
| `CMake Error ... could not find dtc` | devicetree compiler missing | `sudo apt install device-tree-compiler` |
| `Permission denied: '/dev/ttyUSB0'` | user not in `dialout` | `sudo usermod -aG dialout "$USER"`, re-login |
| `esptool: command not found` during `west flash` | flasher not in venv | `pip install esptool==5.3.1` |
| `hal_espressif` symbols missing at link | blobs not fetched | `west blobs fetch hal_espressif` |
| SDK/toolchain version error | wrong or unregistered SDK | `west sdk install --version 1.0.1`, export `ZEPHYR_SDK_INSTALL_DIR` |
| `ZEPHYR_TOOLCHAIN_VARIANT` error | wrong variant | `host` for `native_sim`, `zephyr` for ESP32 |
| `qemu-system-i386: not found` | QEMU host tools missing | install SDK with host tools, export `ZEPHYR_SDK_INSTALL_DIR`; or `sudo apt install qemu-system-x86` |
| Stale binary / "ninja: no work to do" | old build dir | `PRISTINE=1 make zephyr-build-esp32` or `west build -p always` |
| `Address already in use` on 8080 | previous native firmware running | stop it, or set `MRAF_HTTP_PORT=<port>` |

### Windows (WSL2 and native)

| Symptom | Cause | Fix |
|---|---|---|
| `usbipd: error: Access denied` | `bind` needs elevation | run `usbipd bind` in an Administrator PowerShell |
| Device missing from `usbipd list` | USB-UART driver missing | install CP210x/CH340 driver, replug |
| `/dev/ttyUSB0` missing in WSL | device not attached or detached after replug | `usbipd attach --wsl --busid <BUSID>`, `dmesg | tail` |
| `could not open port COM5` | monitor/other tool holds the port | close it, unplug/replug, retry |
| `Activate.ps1 cannot be loaded` | execution policy | `Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser` |
| `west` uses the wrong Python | venv not active | activate venv; `where python`, `where west` |
| `dtc`/`ninja` not found after install | `PATH` not refreshed | close and reopen the terminal |
| `File name too long` / odd path errors | long paths or OneDrive | short path (`C:\mraf`), enable long paths, avoid OneDrive |
| `native_sim` build fails on native Windows | POSIX-only host integration | use WSL2 |
| Slow or flaky builds under `/mnt/c` | Windows filesystem from WSL | move the repository into the WSL home directory |

### Shared

| Symptom | Fix |
|---|---|
| SDK version mismatch after a Zephyr update | `west sdk install --version 1.0.1` |
| Build directory owned by root | `sudo chown -R "$USER" <workspace>` or delete the build dir |
| Out of disk during `west update` | free ~15 GB; use `west update --group-filter` to skip unused vendor HALs |
| Network failures downloading blobs/SDK | retry; corporate proxies need `HTTP_PROXY`/`HTTPS_PROXY` on the host, not only in WSL |

## 8. Related documents

- [05-build-flash.md](05-build-flash.md): build/flash overview and hardware setup
- [14-android-flash.md](14-android-flash.md): flashing from an Android phone, no PC
- [../okf/zephyr-port.md](../okf/zephyr-port.md): Zephyr architecture and memory budget
- [../okf/testing.md](../okf/testing.md): test suites and verified results
- [13-webrtc.md](13-webrtc.md): WebRTC overlay (`webrtc.conf`) and its memory impact
