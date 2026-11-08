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
        if [[ -d "$ESP_XTENSA" ]]; then
            export PATH="$ESP_XTENSA:$ESPTOOL:$PATH"
            export ZEPHYR_TOOLCHAIN_VARIANT=cross-compile
            export CROSS_COMPILE="$ESP_XTENSA/xtensa-esp32-elf-"
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
