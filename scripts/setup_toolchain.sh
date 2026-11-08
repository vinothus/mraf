#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLS_DIR="${MRAF_TOOLS:-/opt/mraf-tools}"
BIN_DIR="$TOOLS_DIR/bin"
CONFIG_FILE="$TOOLS_DIR/arduino-cli.yaml"
ARDUINO_CLI="${ARDUINO_CLI:-$BIN_DIR/arduino-cli}"

mkdir -p "$BIN_DIR" "$TOOLS_DIR/arduino15" "$TOOLS_DIR/Arduino" "$TOOLS_DIR/staging"

if [ ! -x "$ARDUINO_CLI" ]; then
    echo "Installing arduino-cli into $BIN_DIR"
    curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh \
        | BINDIR="$BIN_DIR" sh
fi

if [ ! -f "$CONFIG_FILE" ]; then
    "$ARDUINO_CLI" --config-file "$CONFIG_FILE" config init --overwrite
fi

"$ARDUINO_CLI" --config-file "$CONFIG_FILE" config set directories.data "$TOOLS_DIR/arduino15"
"$ARDUINO_CLI" --config-file "$CONFIG_FILE" config set directories.downloads "$TOOLS_DIR/staging"
"$ARDUINO_CLI" --config-file "$CONFIG_FILE" config set directories.user "$TOOLS_DIR/Arduino"
"$ARDUINO_CLI" --config-file "$CONFIG_FILE" config set board_manager.additional_urls \
    https://espressif.github.io/arduino-esp32/package_esp32_index.json

"$ARDUINO_CLI" --config-file "$CONFIG_FILE" core update-index
for attempt in 1 2 3; do
    if "$ARDUINO_CLI" --config-file "$CONFIG_FILE" core install esp32:esp32; then
        break
    fi
    echo "ESP32 core install attempt $attempt failed; retrying in 10 s"
    sleep 10
done

"$ARDUINO_CLI" --config-file "$CONFIG_FILE" lib install ArduinoJson@7.4.3

"$ARDUINO_CLI" --config-file "$CONFIG_FILE" core list
"$ARDUINO_CLI" --config-file "$CONFIG_FILE" lib list

echo
echo "Toolchain ready."
echo "  export PATH=\"$BIN_DIR:\$PATH\""
echo "  export ARDUINO_CONFIG_FILE=\"$CONFIG_FILE\""
echo "  cd \"$ROOT\" && make compile"
