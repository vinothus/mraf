# 06 - Operation and troubleshooting

## Status LED

| Pattern | Meaning |
|---|---|
| Slow heartbeat (1 s) | Wi-Fi station connected |
| Fast blink (250 ms) | Access point mode (no configured Wi-Fi or connection failed) |
| Rapid blink (120 ms) | Station mode but no link; the controller retries every 30 s |

The status LED is not a fault indicator. Use the dashboard and event log.

## Daily operation

1. Open `http://mraf.local` or the controller's IP address.
2. Log in with the configured credentials.
3. The **Dashboard** shows:
   - mains present / outage time;
   - battery voltage;
   - tank-full and sump-dry states;
   - pump state, current, run time and fault;
   - all 12 relays with mode and schedule state;
   - recent events.
4. Use **Relay** buttons for Auto / On / Off and boost.
5. Use the pump controls for Auto / On / Off, boost and **Reset fault**.
6. Use **All OFF** as an emergency software stop. It does not replace the
   physical E-stop.

Software commands take effect on the next controller tick (about 1 s).

## Modes

- **Auto**: follow the schedule and boost. Safety interlocks remain active.
- **On**: request the load continuously. For the pump, this still respects
  tank-full, sump-dry, minimum-off, recovery delay and fault latches.
- **Off**: force the load off. The pump records the stop time for minimum-off.
- **Boost**: temporarily runs a load in Auto mode for N minutes, capped by
  `max_boost_min` (default 180).

## Default schedules

| Ch | Load | Window | Days |
|---|---|---|---|
| 1 | Shed Light | 18:00-22:00 | All |
| 2 | Yard Light | 18:30-05:30 | All |
| 3 | Drip Zone A | 06:00-06:30 | Mon/Wed/Fri |
| 4 | Drip Zone B | 06:30-07:00 | Mon/Wed/Fri |
| 5 | Aerator 1 | 22:00-04:00 | All |
| 6 | Aerator 2 | 22:00-04:00 | All |
| 7 | Cattle Fan | 10:00-16:00 | All |
| 8 | Milking Light | 04:30-06:00 | All |
| 9 | Gate Light | 19:00-06:00 | All |
| 10-12 | Spares | Disabled | - |
| Pump | 5 HP borewell | 06:00-07:00 and 17:00-18:00 | All |

Edit the windows in **Schedules**. Use the day checkboxes to build the day mask.
A window that crosses midnight belongs to the day it starts; the morning part
uses the previous day's mask.

## Pump behaviour

- The pump starts only inside a schedule/boost or in manual On, subject to
  minimum-off, recovery delay and safety interlocks.
- The pump stops immediately on tank full, sump dry, mains loss, mode Off or a
  latched fault.
- A dry-run or maximum-run fault stays latched until **Reset fault** is pressed
  or `POST /api/pump/reset` is called. Find and fix the cause first.
- If the controller detects current while it is not commanding the pump, it
  shows `EXTERNAL`. This usually means someone used the HAND starter. The
  controller will not fight it.
- After a power cut:
  - all relays drop because their coils are on the mains-derived rail;
  - the ESP32 stays alive on the battery;
  - the pump waits for the recovery delay after mains returns;
  - if **Resume after power returns** is off, the pump waits for a new schedule
    edge instead of restarting in the middle of a window.

## Fault messages

| Message | Meaning | Action |
|---|---|---|
| `FAULT: dry run / no current` | Pump is commanded on but current stayed below threshold after grace | Check prime, suction, valve, sensor wiring; reset after fixing |
| `FAULT: max run time` | Pump ran longer than `max_run_min` | Check the schedule, tank level and whether the pump is actually moving water; reset |
| `tank full` | Tank-full sense float active | Pump stops; clears when the float clears |
| `sump dry` | Sump-dry sense float active | Pump stops; clears when the float clears |
| `NO POWER` | Mains-derived rail is dead | Check isolator, RCCB, MCB, SMPS and mains sense divider |
| `EXTERNAL` | Current detected without a controller command | Check the HAND/auto selector and contactor latch |
| `WAITING` | The pump wants to run but a timer/interlock is holding it | Check minimum-off, recovery delay, tank/sump and fault state |

## Outage and battery

- The battery-backed rail powers only the ESP32, RTC and sensors.
- The relay coils and pump are not battery-backed. All loads drop on mains loss.
- The dashboard remains reachable while the battery and Wi-Fi are alive.
- The LVD disconnects the LiFePO4 pack around 11 V. If the controller goes
  dark during a long outage, recharge the pack and check its BMS and health.
- A 12.8 V 6 Ah LiFePO4 pack typically gives 3-5 days of controller runtime.
  Measure your actual current if runtime matters.
- After a long outage, check the RTC/time and the first pump start.

## Maintenance

Monthly:

- Press the RCCB test button.
- Inspect the enclosure for moisture, insects and loose terminals.
- Check the event log for repeated faults or outages.
- Confirm the LiFePO4 charger profile (14.4-14.6 V charge, 13.5-13.6 V float).
- Test the E-stop and float safety chain with the pump disconnected.

Yearly:

- Replace the RTC CR2032 if the time drifts after unplugging.
- Load-test the LiFePO4 pack and inspect the BMS; replace if capacity drops.
- Check contactor contacts and overload settings.
- Torque mains terminals and inspect for discoloration.
- Clean the enclosure vents and check cable glands.

## Troubleshooting

| Symptom | Check |
|---|---|
| No dashboard | Power, LED pattern, Wi-Fi credentials, router isolation, IP address, `mraf.local` mDNS |
| AP mode every boot | Wrong Wi-Fi password, 5 GHz-only router, weak signal, or MAC filtering |
| Relay clicks but load does not run | COM/NO wiring, load breaker, neutral, relay contact rating |
| Relay does not click | JD-VCC jumper, 5 V SMPS, IN pin mapping, active-low logic, GPIO conflict |
| Pump does not start in AUTO | Schedule/day mask, mode, tank/sump, minimum-off, recovery delay, fault latch, phase-failure relay |
| Pump starts then trips | Overload setting, phase loss, low voltage, dry run, mechanical load |
| Current reads zero while running | ACS712 supply, divider, GPIO39, zero calibration (boot with pump off) |
| Current reads high at rest | ACS712 zero drifted; reboot with pump off; check divider |
| Time wrong | NTP blocked, RTC missing/dead battery, timezone, manual time API |
| Random reboots | 5 V supply, brownout during relay switching, USB/buck back-feed, loose ground |
| Wi-Fi drops | Distance, antenna clearance, metal enclosure, set `WiFi.setSleep(false)` already enabled |
| Config save fails | Validation error in the JSON; check the event/API error message |
| Forgot password | Erase NVS/flash and reconfigure (see docs/05) |

## Emergency stop

Use the physical E-stop. The dashboard's **All OFF** is a convenience, not a
safety function. If the pump does not stop:

1. Press the E-stop.
2. Open the lockable isolator.
3. Do not open the enclosure while the isolator is on.
