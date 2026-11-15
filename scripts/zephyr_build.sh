#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="$ROOT/firmware/zephyr"
TARGET="${1:-native}"

ZEPHYR_VENV="${ZEPHYR_VENV:-/opt/zephyr-venv}"
ZEPHYR_BASE="${ZEPHYR_BASE:-/opt/zephyrproject/zephyr}"
ZEPHYR_TOOLS="${ZEPHYR_TOOLS:-/opt/mraf-tools/arduino15/packages/esp32/tools}"
ESP_XTENSA="${ESP_XTENSA:-$ZEPHYR_TOOLS/esp-x32/2601/bin}"
ESPTOOL="${ESPTOOL:-$ZEPHYR_TOOLS/esptool_py/5.3.1}"
ZEPHYR_SDK_INSTALL_DIR="${ZEPHYR_SDK_INSTALL_DIR:-/opt/zephyr-sdk-1.0.1}"

if [[ -f "$ZEPHYR_VENV/bin/activate" ]]; then
    # shellcheck disable=SC1091
    source "$ZEPHYR_VENV/bin/activate"
fi

export ZEPHYR_BASE
EXTRA_ARGS=()
if [[ "${PRISTINE:-0}" == "1" ]]; then
    EXTRA_ARGS+=(-p always)
fi

case "$TARGET" in
    native)
        export ZEPHYR_TOOLCHAIN_VARIANT=host
        BOARD="native_sim/native/64"
        BUILD_DIR="$APP/build/native"
        ;;
    esp32)
        export PATH="$ESPTOOL:$PATH"
        ZEPHYR_SDK_XTENSA="$ZEPHYR_SDK_INSTALL_DIR/gnu/xtensa-espressif_esp32_zephyr-elf"
        if [[ ! -x "$ZEPHYR_SDK_XTENSA/bin/xtensa-espressif_esp32_zephyr-elf-gcc" ]]; then
            ZEPHYR_SDK_XTENSA="$ZEPHYR_SDK_INSTALL_DIR/xtensa-espressif_esp32_zephyr-elf"
        fi
        if [[ -x "$ZEPHYR_SDK_XTENSA/bin/xtensa-espressif_esp32_zephyr-elf-gcc" ]]; then
            # Preferred: the Zephyr SDK toolchain ships a Zephyr-compatible libc.
            export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
            export ZEPHYR_SDK_INSTALL_DIR
        elif [[ -d "$ESP_XTENSA" ]]; then
            # Fallback: the Arduino ESP32 cross toolchain.
            export PATH="$ESP_XTENSA:$PATH"
            export ZEPHYR_TOOLCHAIN_VARIANT=cross-compile
            export CROSS_COMPILE="$ESP_XTENSA/xtensa-esp32-elf-"
            export CROSS_COMPILE_TOOLCHAIN_PATH="${CROSS_COMPILE_TOOLCHAIN_PATH:-$ESP_XTENSA/..}"
        fi
        BOARD="esp32_devkitc/esp32/procpu"
        BUILD_DIR="$APP/build/esp32"
        ;;
    *)
        echo "usage: $0 [native|esp32]" >&2
        exit 2
        ;;
esac

echo "== building Zephyr firmware for $BOARD =="
west build -s "$APP" -b "$BOARD" -d "$BUILD_DIR" "${EXTRA_ARGS[@]}"
echo "== build complete: $BUILD_DIR/zephyr =="
