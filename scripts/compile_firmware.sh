#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLS_DIR="${MRAF_TOOLS:-/opt/mraf-tools}"
CONFIG_FILE="${ARDUINO_CONFIG_FILE:-$TOOLS_DIR/arduino-cli.yaml}"
ARDUINO_CLI="${ARDUINO_CLI:-$TOOLS_DIR/bin/arduino-cli}"
FQBN="${FQBN:-esp32:esp32:esp32}"
BUILD_DIR="${BUILD_DIR:-/tmp/mraf-build}"

cd "$ROOT"
python3 tools/gen_webui.py

CLI_ARGS=()
if [ -f "$CONFIG_FILE" ]; then
    CLI_ARGS+=(--config-file "$CONFIG_FILE")
fi

"$ARDUINO_CLI" "${CLI_ARGS[@]}" compile \
    --fqbn "$FQBN" \
    --build-path "$BUILD_DIR" \
    --warnings default \
    firmware/mraf

echo
echo "Build output: $BUILD_DIR"
find "$BUILD_DIR" -maxdepth 1 -name '*.bin' -printf '%f %s bytes\n' 2>/dev/null || true
