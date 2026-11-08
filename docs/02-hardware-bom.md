# 02 - Hardware, sizing and BOM

## Cost summary

Prices are indicative Chennai retail prices (2026) and exclude labour, conduit,
civil work and GST variations. The full line-item CSV is
[`hardware/bom.csv`](../hardware/bom.csv).

| Package | Approx. cost |
|---|---:|
| Controller + relay board + sensors + battery + panel + pump starter | ₹15,620 |
| Recommended RCCB + phase-failure relay | ₹2,450 |
| **Total with recommended safety items** | **₹18,100** |

The controller itself (ESP32, relay board, power rails, sensors, RTC, battery)
is about ₹5,000-6,000. Most of the cost is the pump starter, enclosure and
cabling.

## 5 HP pump sizing

Assume:

- 5 HP = 3.73 kW shaft power
- 415 V three-phase
- power factor 0.85
- motor efficiency 0.88

\[
I_{FLC} = \frac{3730}{\sqrt{3}\times 415\times 0.85\times 0.88}
        \approx 6.9\text{ A}
\]

| Item | Selection | Reason |
|---|---|---|
| Contactor | 9 A AC-3, 3-pole, coil 230 V or 415 V | AC-3 rating covers 3.7 kW at 400-415 V; do not use the relay board for motor current |
| Thermal overload | 5.5-8 A, set about 7 A | Protects against sustained overload and single phasing |
| MCB / MPCB | 16 A, C or D curve, 3-pole | Starting current 6-8× FLC; D curve or motor-rated MPCB is preferable |
| Cable | 2.5 mm² Cu 4-core armored, 4 mm² for long runs | Voltage drop and starting current |
| Isolator | 32 A, 3-pole + N, lockable | Service isolation |
| RCCB | 40 A, 4-pole, 30 mA (100 mA if nuisance trips) | Earth leakage protection |
| Phase failure | 3-phase phase-failure / phase-sequence relay | Prevents single-phasing the motor |
| E-stop | Latching mushroom, NC | Hardwired in the coil circuit |
| Coil suppression | RC snubber + MOV | Protects contacts and reduces EMI |

The ESP32 never switches the pump directly. It closes a small relay contact in
the 230 V contactor-coil circuit. The contactor carries the motor current.

## Controller and I/O

| Ref | Part | Approx. price | Notes |
|---|---|---:|---|
| U1 | ESP32 DevKit V1 38-pin | ₹450 | Buy a board with USB and 5 V input; CP2102/CH340 is fine |
| U2 | 16-channel 5 V relay module | ₹850 | Active-LOW, optocoupler inputs, JD-VCC jumper |
| U3 | 5 V 3 A SMPS | ₹550 | Relay coils only; 13 coils at ~20 mA is well within 3 A |
| U4 | 5 V 3 A buck converter | ₹150 | Battery-backed controller rail |
| U5 | 12.8 V 6 Ah LiFePO4 | ₹1,700 | Controller only, not the pump; built-in BMS |
| U6 | 14.6 V 2 A LiFePO4 charger | ₹700 | 14.4-14.6 V charge; 13.5-13.6 V float |
| U7 | 12 V low-voltage disconnect | ₹200 | Cut off around 11 V; reconnect around 12.8 V |
| U8 | DS3231 RTC | ₹120 | CR2032 backup |
| U9 | ACS712-30 A | ₹180 | 2:1 divider required |
| U10-U13 | 4 float switches | ₹600 | 2 NC safety floats + 2 NO sense floats |
| U14-U20 | Resistors, fuse, snubber, terminals, DIN, ferrules, labels | ₹1,580 | Small parts add up |

## Power architecture

There are two separate 5 V rails:

1. **Mains-derived relay rail**
   - 230 V AC → 5 V 3 A SMPS.
   - Feeds the relay module's `JD-VCC` and all coil returns.
   - When mains fails, this rail collapses and every relay drops out.
   - Add a 1 A fuse on the SMPS input and a MOV across its output.

2. **Battery-backed controller rail**
   - Mains → LiFePO4 charger → 12.8 V battery; battery + → fuse → LVD →
     5 V buck → ESP32 5 V pin. The charger is not in series with the load.
   - Powers the ESP32, RTC and low-voltage sensors only.
   - The pump and relay coils are never on this rail.
   - The LVD protects the LiFePO4 pack from deep discharge.
   - Never use a lead-acid float charger on LiFePO4: use a LiFePO4 profile.

The logic side of the relay module is powered from the ESP32 3.3 V pin, and the
JD-VCC jumper is removed. The relay module's logic ground and the ESP32 ground
are common. The coil-side ground is the SMPS ground. With the jumper removed,
the optocouplers keep the two sides isolated.

### Battery runtime estimate

A 12.8 V 6 Ah LiFePO4 pack has about 77 Wh. The BMS and LVD leave roughly
60-68 Wh usable. The ESP32 with Wi-Fi active, the RTC and sensors average
roughly 0.4-0.8 W, so expect approximately **3-5 days**, depending on Wi-Fi
traffic, sensor current and battery age. Measure the actual current before
relying on a runtime figure. This is not a pump backup.

## Sensor scaling

### Battery voltage

- Divider: 100 kΩ top, 22 kΩ bottom.
- Ratio: (100 + 22) / 22 = 5.545.
- 14.4 V → 2.60 V at the ADC. Safe for 3.3 V.

### Mains presence

- Sense the **5 V relay-rail output**, not 230 V directly.
- Divider: 22 kΩ top, 15 kΩ bottom.
- Ratio: (22 + 15) / 15 = 2.467.
- 5 V → 2.03 V at GPIO32. HIGH = mains present.
- If the 5 V SMPS fails, the input reads LOW even if mains is present. This is a
  useful fault indication.

### Pump current

- ACS712-30 A output: 66 mV/A, zero at Vcc/2 = 2.5 V.
- Use a matched 10 kΩ / 10 kΩ divider before GPIO39.
- Effective sensitivity: 66 / 2 = **33 mV/A**.
- At 7 A: 231 mV above zero.
- At 50 A starting current: output saturates near 5 V, divided to about 2.5 V.
  The firmware uses running current for dry-run detection, so saturation during
  start is acceptable.

### Floats

Use four separate floats:

| Function | Contact | Wiring |
|---|---|---|
| Tank-full safety | NC | In series with the 230 V contactor coil chain; opens when full |
| Tank-full sense | NO | COM to GPIO34, NO to GND, 10 kΩ pull-up to 3.3 V; LOW = full |
| Sump-dry safety | NC | In series with the 230 V contactor coil chain; opens when dry |
| Sump-dry sense | NO | COM to GPIO35, NO to GND, 10 kΩ pull-up to 3.3 V; LOW = dry |

Never share a float common between the 230 V safety circuit and the 3.3 V
sense circuit. Two cheap floats are safer than one mixed-voltage contact.

## Sourcing notes

- **Ritchie Street / Parry's Corner**: contactors, overloads, MCBs, RCCBs,
  enclosures, DIN rail, terminals, cable, float switches, panel lamps.
- **Robu / Element14 / Amazon**: ESP32, relay module, sensors, RTC, buck, LVD,
  charger.
- Buy a relay module with optocouplers and a real JD-VCC jumper. Check that the
  relay contacts are rated for the load (10 A resistive is common; inductive
  loads derate).
- Buy a 5 V SMPS from a known brand. Cheap no-name units often fail short and
  can back-feed the ESP32.
- Confirm the contactor coil voltage before wiring. A 415 V coil needs two
  phases; a 230 V coil needs phase + neutral.
- Use genuine RCCBs and MCBs. Test the RCCB test button before commissioning.
- Keep spares: one relay module, one buck converter, fuses and a CR2032.

## Alternatives

| Need | Cheaper | Better |
|---|---|---|
| Relay board | 12-channel board | 16-channel with optocouplers and replaceable relays |
| Pump starter | Direct MCB + contactor | MPCB + contactor + phase-failure relay |
| Timekeeping | NTP only | NTP + DS3231 |
| Current sensing | None | ACS712-30 A (recommended) |
| Wi-Fi provisioning | Hard-coded credentials | Dashboard Wi-Fi scan and AP fallback |
| Battery | 12.8 V 6 Ah LiFePO4 | 12.8 V 10 Ah LiFePO4 with a larger BMS |
