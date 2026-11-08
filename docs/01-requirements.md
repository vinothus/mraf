# 01 - Requirements and scope

## Goal

Build a practical, cost-effective farm controller that runs 12 scheduled loads
and a 5 HP three-phase borewell pump from an ESP32, with a web dashboard, safe
pump protection, battery-backed control electronics, and a simulator that can
be used for training and testing before touching mains wiring.

## Functional requirements

| ID | Requirement |
|---|---|
| F1 | Control 12 independent 230 V loads through relays. |
| F2 | Give each channel at least 4 daily on/off windows with a Monday-Sunday day mask. |
| F3 | Allow per-channel Auto / On / Off and a timed boost (up to 180 min default). |
| F4 | Control a 5 HP 3-phase pump through a contactor, not directly through the relay board. |
| F5 | Provide pump Auto / On / Off, scheduled windows, minimum off time and maximum run time. |
| F6 | Detect pump dry-run using current, with a configurable grace period. |
| F7 | Accept tank-full and sump-dry float inputs and stop the pump on either condition. |
| F8 | Detect mains loss, log it, drop all loads, and optionally resume the pump after a recovery delay. |
| F9 | Keep the ESP32, RTC and sensors alive on a 12.8 V LiFePO4 battery during a mains outage. |
| F10 | Serve a local dashboard and REST API over Wi-Fi with Basic authentication. |
| F11 | Fall back to a Wi-Fi access point when the configured network is unavailable. |
| F12 | Keep time with NTP and a DS3231 RTC; support manual time setting. |
| F13 | Persist configuration and runtime modes in NVS across reboots. |
| F14 | Provide a native simulator with the same API and controller behaviour. |
| F15 | Provide wiring diagrams, a BOM, build instructions, operating docs and a video script. |
| F16 | Poll a remote Java server over CoAP for control, data and schedule requests, with a shared token, device credentials, registration and next-poll acknowledgement. |
| F17 | Provide a Spring Boot server that manages multiple ESP32 devices, stores registrations, configs and queues in an embedded H2 database, and serves an admin fleet view plus per-device dashboards. |
| F18 | Support device profiles (`agri`, `home`, `godown`, `industry`) that map relays to named zones and equipment (`valve`, `light`, `motor`, `fan`, `pump`, `socket`, `heater`, `door`, `other`). |
| F19 | Support momentary pulses for motors and contactors, and daily/weekly/monthly/yearly recurring schedules, from the ESP32 and the server. |
| F20 | Require an administrator to confirm a newly registered device before it can be controlled, and let the server vary each device's profile and poll interval. |
| F21 | Optionally expose confirmed server devices to Matter controllers through a Node.js bridge, with relays/pump as OnOff endpoints and tank/sump floats as contact sensors, without changing the ESP32 firmware. |

## Non-functional requirements

| ID | Requirement |
|---|---|
| N1 | No custom PCB fabrication. Use off-the-shelf modules and DIN terminals. |
| N2 | Components available in Chennai (Ritchie Street, Parry's Corner, local electrical dealers, Robu/Element14/Amazon). |
| N3 | Control electronics must survive a mains outage; the pump does not need to run on battery. |
| N4 | All mains switching must be through proper contactors, breakers and a hardwired safety chain. |
| N5 | The controller must be usable from a phone on the local network. |
| N6 | The design must be understandable and repairable by a local electrician. |
| N7 | Firmware must compile with the Arduino-ESP32 core and ArduinoJson only (plus built-in libraries). |
| N8 | The simulator must run with the Python standard library only. |
| N9 | The BOM should stay near ₹16,000 for the core system, excluding labour. |

## Design constraints

- Supply: 415 V, 50 Hz, three-phase + neutral + earth.
- Pump: 5 HP (3.73 kW), full-load current about 6.9 A at 415 V, direct-on-line
  starting current about 6-8 times FLC.
- Control electronics: 3.3 V ESP32, 5 V relay module, 12.8 V LiFePO4 battery rail.
- No internet dependency at runtime. NTP is used when available.
- Single enclosure, DIN rail, standard panel wiring.
- The ESP32 is not a safety device. Hardwired protection is mandatory.

## Assumptions

- The pump is a standard 3-phase induction motor with a DOL-starter-compatible
  load. If the motor is larger, or the supply is weak, use a star-delta or
  soft starter and recalculate the contactor/overload.
- A neutral is available for the 230 V control circuit. If not, use a
  phase-to-phase control transformer or a 415 V coil contactor.
- The tank and sump floats can be installed with separate safety (NC, mains)
  and sense (NO, 3.3 V) contacts. Do not mix 230 V and 3.3 V on one common.
- A licensed electrician will inspect and commission the mains side.
- The controller battery is a 12.8 V LiFePO4 pack with a LiFePO4 charger and
  low-voltage disconnect. Only the controller, RTC and sensors run from it.

## Out of scope

- Solar pumping, VFD/soft starter control, and three-phase phase-angle control.
- Automatic chlorination, fertigation dosing, or flow-total measurement.
- Cloud dashboards, MQTT, mobile apps, OTA firmware updates.
- Certified product enclosure, surge protection for the whole farm, or
  lightning protection design.
- Revenue-grade energy metering.

## Acceptance criteria

1. Simulator: all schedule, pump, safety and API tests pass with `make test`.
2. Firmware: `make compile` produces a binary for `esp32:esp32:esp32`.
3. With the pump disconnected, each relay output can be toggled from the
   dashboard and by schedule.
4. With the pump connected through the contactor:
   - pump starts only inside a scheduled window or in manual On;
   - tank-full and sump-dry stop the pump immediately;
   - a simulated phase loss or E-stop drops the contactor;
   - an outage drops all relays and the controller stays reachable on battery;
   - after power returns, the pump waits for the recovery delay and the
     configured resume policy.
5. The dashboard loads on a phone, schedules survive a reboot, and the API
   rejects unauthenticated requests when auth is configured.
6. With CoAP enabled, a device registers with its own credentials, waits for
   admin confirmation, then polls a remote server for data, control, config and
   profile/poll-interval changes. The server can queue a pulse control or a
   recurring schedule, and receives the acknowledgement on the following poll.
7. With the optional Matter bridge running, a confirmed device appears as an
   aggregator with 12 relay endpoints, a pump endpoint and two contact sensors;
   a Matter on/off command reaches the server control queue, and startup state
   sync does not enqueue commands.

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| Lethal mains contact | Licensed electrician; isolator; RCCB; covered terminals; test with pump disconnected. |
| Relay board trace overheats on pump current | Pump is switched by a contactor; relay board only drives the coil. |
| Dry run destroys pump | ACS712 current trip, grace period, sump float, and hardwired float in the coil chain. |
| Nuisance RCCB trips on pump start | Use 100 mA RCCB if 30 mA trips, with local rules and electrician advice. |
| ESP32 brownout during relay switching | Separate 5 V SMPS for coils; decoupling; common logic ground only. |
| Wi-Fi unavailable | AP fallback; local dashboard; no cloud dependency. |
| Time wrong after long outage | DS3231 backup battery; NTP when network returns; manual time API. |
| SD card / cloud failures | None used. Configuration is in NVS. |
