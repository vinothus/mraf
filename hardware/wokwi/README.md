# Wokwi quick check

This is a small browser-only wiring check, not the full digital twin. The full
controller (12 schedules, pump state machine, REST API, accelerated clock) is in
`simulator/mraf_sim.py`.

## Build for Wokwi

```bash
make wokwi
```

That compiles the real firmware and writes `mraf.ino.bin` / `mraf.ino.elf` into
`hardware/wokwi/build/`. Open this folder in the Wokwi VS Code extension, or
upload `diagram.json`, `wokwi.toml` and the `build/` folder to a Wokwi project.

## What the diagram shows

| Wokwi part | ESP32 pin | Purpose |
|---|---|---|
| LED 1 (yellow) | GPIO13 | Channel 1 / Shed Light |
| LED 2 (yellow) | GPIO14 | Channel 2 / Yard Light |
| LED 3 (green) | GPIO16 | Channel 3 / Drip A |
| LED 4 (green) | GPIO17 | Channel 4 / Drip B |
| LED 5 (red) | GPIO33 | Pump contactor command |
| Potentiometer | GPIO39 | ACS712 current signal |
| Pushbutton (green) | GPIO32 | Mains present (hold) |
| Pushbutton (blue) | GPIO34 | Tank full (hold) |
| Pushbutton (orange) | GPIO35 | Sump dry (hold) |

The float buttons are momentary. Hold one to simulate the condition; release to
clear it. The tank/sump inputs use external 10 k pull-ups in the diagram, the
same as the real panel.

The ACS712 output is simulated by the potentiometer. At boot the firmware
calibrates the zero point with the pump OFF, so leave the pot near mid-scale at
reset. Increase it after the pump starts to represent running current.

## Limits

- Wokwi has no 3-phase motor, contactor, float switch or 16-channel relay board.
  The LEDs only show the ESP32 output state.
- Wi-Fi is not available in the default Wokwi ESP32 simulation, so the firmware
  will start its AP fallback. This is expected.
- Use the native simulator for API, schedule and pump logic testing.
