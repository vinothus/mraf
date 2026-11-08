# 03 - Wiring and panel build

> **Danger:** 230/415 V AC is lethal. Work with the isolator off and verified.
> Use a licensed electrician for the mains, contactor and pump wiring. The ESP32
> is not a safety device; the hardwired safety chain is mandatory.

## Diagrams

| Diagram | File |
|---|---|
| System blocks and power rails | [`hardware/diagrams/system-block.svg`](../hardware/diagrams/system-block.svg) |
| ESP32, relay board, sensors and rails | [`hardware/diagrams/control-wiring.svg`](../hardware/diagrams/control-wiring.svg) |
| 5 HP DOL starter and safety chain | [`hardware/diagrams/pump-starter.svg`](../hardware/diagrams/pump-starter.svg) |
| Suggested panel layout | [`hardware/diagrams/panel-layout.svg`](../hardware/diagrams/panel-layout.svg) |

## Pin map

| GPIO | Direction | Connected to | Notes |
|---|---|---|---|
| 13 | Output | Relay IN1 | Shed Light |
| 14 | Output | Relay IN2 | Yard Light |
| 16 | Output | Relay IN3 | Drip Zone A |
| 17 | Output | Relay IN4 | Drip Zone B |
| 18 | Output | Relay IN5 | Aerator 1 |
| 19 | Output | Relay IN6 | Aerator 2 |
| 21 | Output | Relay IN7 | Cattle Fan |
| 22 | Output | Relay IN8 | Milking Light |
| 23 | Output | Relay IN9 | Gate Light |
| 25 | Output | Relay IN10 | Spare 1 |
| 26 | Output | Relay IN11 | Spare 2 |
| 27 | Output | Relay IN12 | Spare 3 |
| 33 | Output | Relay IN13 | Pump contactor |
| 32 | Input | 5 V SMPS → 22k/15k divider | HIGH = mains present |
| 34 | Input | Tank-full sense float | 10k pull-up; LOW = full |
| 35 | Input | Sump-dry sense float | 10k pull-up; LOW = dry |
| 36 | Input | 12 V battery → 100k/22k divider | ADC |
| 39 | Input | ACS712 → 10k/10k divider | ADC |
| 4 | I2C SDA | DS3231 SDA | 3.3 V logic |
| 5 | I2C SCL | DS3231 SCL | 3.3 V logic |
| 2 | Output | Status LED | Active HIGH on most boards |

Use only ADC1 pins (32-39) for analog inputs. ADC2 is unavailable while Wi-Fi is
active.

## Relay module modification

1. Remove the **JD-VCC jumper**.
2. Connect `VCC` to ESP32 **3.3 V**.
3. Connect `GND` to ESP32 **GND**.
4. Connect `JD-VCC` to the **5 V SMPS +**.
5. Connect the relay module's coil-side `GND` to **5 V SMPS -**.
6. Connect `IN1..IN13` to the GPIOs in the pin map.

The inputs are active LOW: driving the pin LOW turns the relay ON. The firmware
handles this with `RELAY_ACTIVE_LOW = true`.

Use the relay **COM/NO** contacts for loads. Keep all mains terminals covered.
For inductive loads (fans, aerators), add an RC snubber or MOV across the load
where practical.

## Power rails

### Mains-derived 5 V relay rail

```text
230 V L -> 1 A fuse -> 5 V 3 A SMPS L
230 V N ->             5 V 3 A SMPS N
5 V +  -> JD-VCC and relay coil +
5 V -  -> relay coil - and coil-side GND
```

Add a MOV across the SMPS output and keep this rail away from the ESP32 3.3 V
and signal wiring.

### Battery-backed controller rail

```text
LiFePO4 charger + -> 12.8 V LiFePO4 +
LiFePO4 charger - -> 12.8 V LiFePO4 -
12.8 V LiFePO4 + -> 5 A fuse -> LVD IN+
12.8 V LiFePO4 - -> LVD IN-
LVD OUT+ -> 5 V buck IN+
LVD OUT- -> 5 V buck IN- -> common controller GND
5 V buck OUT+ -> ESP32 5 V pin
5 V buck OUT- -> ESP32 GND
```

The ESP32 board regulates the 5 V input down to 3.3 V internally. Do not feed
the bare 12.8 V pack into the 5 V pin.

The charger and battery are in parallel. The charger is not in series with the
load.

- Use a LiFePO4 charger: 14.4-14.6 V charge, 13.5-13.6 V float.
- Do not use a lead-acid float charger on a LiFePO4 pack.
- Set the LVD to disconnect around 11 V and reconnect around 12.8 V.
- Do not connect the buck output in parallel with the 5 V SMPS.
- The ESP32 can also be powered from USB during bench testing; remove the buck
  output or the USB connection so the two sources do not fight.
- Fuse the battery positive close to the battery.

## Sensors

### Battery voltage

```text
12 V + --- 100k ---+--- GPIO36
                   |
                  22k
                   |
                  GND
```

### Mains presence

```text
5 V SMPS + --- 22k ---+--- GPIO32
                      |
                     15k
                      |
                     GND
```

This senses the relay-rail 5 V, not the 230 V waveform. It tells the controller
whether the mains-derived rail is alive. A loss of the 5 V SMPS also reads as a
mains loss, which is the safe interpretation.

### ACS712 pump current

```text
Pump L1 wire passes through the ACS712 sensor.
ACS712 VCC -> 5 V (battery rail)
ACS712 GND -> GND
ACS712 OUT -> 10k ---+--- 10k --- GND
                     |
                   GPIO39
```

Use matched 10 kΩ resistors. The firmware calibrates the zero point at boot;
the pump must be OFF during boot for the best calibration.

### Float switches

Use separate safety and sense floats:

```text
Tank-full sense:  3.3 V --- 10k ---+--- GPIO34
                                   |
                                 float NO
                                   |
                                  GND

Sump-dry sense:   3.3 V --- 10k ---+--- GPIO35
                                   |
                                 float NO
                                   |
                                  GND
```

The separate NC safety floats go in the 230 V contactor coil chain. Never use
one float's common for both 230 V and 3.3 V circuits.

### DS3231 RTC

```text
DS3231 VCC -> 3.3 V
DS3231 GND -> GND
DS3231 SDA -> GPIO4
DS3231 SCL -> GPIO5
```

Fit a fresh CR2032. The firmware detects the RTC at boot; if it is absent, the
controller still runs and waits for NTP or manual time.

## Contactor and safety chain

The ESP32 pump relay is a **dry contact in the contactor coil circuit**. It does
not carry motor current.

```text
230 V L -> 6 A control MCB -> safety chain -> AUTO/HAND selector -> coil A1
coil A2 -> N
```

The safety chain is a series of **NC** contacts:

1. E-stop (latching mushroom).
2. Thermal overload NC (95-96).
3. Phase-failure relay NC.
4. Tank-full safety float NC.
5. Sump-dry safety float NC.

Any one opening de-energises the contactor coil. Add an RC snubber and MOV
across A1/A2.

### AUTO / OFF / HAND

- **OFF**: both control paths open.
- **AUTO**: the ESP32 relay contact (IN13) closes the coil circuit when the
  firmware wants the pump.
- **HAND**: local STOP NC and START NO pushbuttons with the contactor's auxiliary
  NO contact as a latch. Use HAND only for maintenance and keep the tank/sump
  safety chain in circuit.

Do not wire a selector position that bypasses the safety chain.

## Panel layout and wiring practice

- Use a 500 × 400 × 200 mm IP65 enclosure.
- DIN rail 1: isolator, RCCB, pump MCB, contactor, overload, phase-failure
  relay, control MCB, 5 V SMPS.
- DIN rail 2: terminals, relay board, fuse, buck, LVD, more terminals.
- Mount the ESP32 on standoffs with the antenna edge clear of metal.
- Keep 230/415 V wiring in one duct and signal wiring in another.
- Use bootlace ferrules on every stranded wire and label both ends.
- Bond the enclosure, DIN rail and motor frame to earth.
- Use cable glands at the bottom; add a small vent/silica pack to reduce
  condensation. Vent the battery compartment, but keep the LiFePO4 pack dry.
- Leave service loops and do not cross mains wiring over the ESP32 antenna.

## Commissioning checklist

1. **Insulation and earth**: electrician checks insulation resistance and earth
   continuity with the controller disconnected.
2. **Control only**: energise the control circuit with the pump disconnected.
   Verify E-stop, overload trip and float chain drop the contactor.
3. **Relay outputs**: use the dashboard to toggle each relay and confirm the
   correct LED/relay clicks.
4. **Sensors**: short the sense floats to GND and confirm the dashboard shows
   tank full / sump dry. Open the safety floats and confirm the contactor drops.
5. **Current**: run the pump in HAND briefly, confirm the dashboard current is
   plausible (about 7 A), then stop and check for leaks/abnormal noise.
6. **Schedule**: set a short test window, confirm AUTO start/stop and the
   minimum-off timer.
7. **Outage**: with the pump running, switch off the mains. Confirm all relays
   drop, the ESP32 stays on battery, and the dashboard still responds. Restore
   mains and confirm the recovery delay.
8. **Auth**: change the default password and confirm unauthenticated API calls
   are rejected.
