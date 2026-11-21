# Production E2E Testing

Last run: 2026-11-21 (Asia/Kolkata)

This document records the production-release checks for the ESP32 controller,
the Zephyr firmware, the embedded dashboard/MCP endpoint and the public Spring
Boot server at `https://farm.ritebussiness.com`.

## Targets

| Component | Target | Notes |
|---|---|---|
| Controller | `http://192.168.0.201` | Live ESP32 on the farm LAN |
| Controller hostname | `mraf` | Wi-Fi/mDNS hostname |
| Remote CoAP host | `152.67.176.110` | UDP `5683`, path `/mraf`; DNS name `farm.ritebussiness.com` is fallback |
| Remote device ID | `ESP32-B03FD3` | Name: `Neer Kaavalan` |
| Public server | `https://farm.ritebussiness.com` | nginx → `127.0.0.1:8081` |
| Server health | `/api/health` | Public endpoint |
| Device credentials | From `/etc/mraf/mraf.env` | Never commit secrets |

## Release artifacts

| Artifact | SHA256 |
|---|---|
| `firmware/zephyr/build/esp32/zephyr/zephyr.bin` | `7061566a2a99bbf002051dad6b5c6daa8b56e66d48130be6c99a703a2735c9e2` |
| `server/target/mraf-server-1.0.0.jar` | `d2c1c98a49d5b1340b2d17ca510a49558f0a235b358812d9b54afb38d2ba86cb` |

The ESP32 image includes persistent time retention, larger CoAP buffers, MCP
schema preservation, CoAP send retry/errno diagnostics, a smaller remote data
payload, additional Zephyr network buffers, separate START/STOP GPIO status,
separate START/STOP pulse durations, active-profile refresh from
`/api/config`, a stripped local profiles cache on settings save, and a 16 KiB
HTTP response buffer with truncation detection. The server jar is unchanged and
includes server epoch in every CoAP response plus the transaction fix for queue
clearing.

## Automated checks

Command:

```bash
make server-test server-build ui test zephyr-build zephyr-e2e
```

Result: PASS

| Suite | Result |
|---|---|
| Spring Boot server tests | PASS |
| C++ schedule tests | PASS |
| C++ controller tests | PASS |
| Python simulator tests | 53/53 PASS |
| Zephyr native_sim E2E | 18/18 PASS |

The Zephyr E2E suite includes `test_99_time_survives_restart`, which sets a
known epoch, terminates the firmware, starts it again in the same storage
directory, and verifies that the restored clock is valid and close to the saved
epoch. After the MCP/CoAP fix, `test_14_mcp` also asserts the full
`control_relay` schema. Final validation after that fix: server tests PASS,
C++ tests PASS, Python 53/53 PASS, Zephyr native_sim E2E 18/18 PASS and ESP32
build PASS.

## Time retention design

Boot clock source order:

1. DS3231 RTC, when present and plausible.
2. Persistent epoch stored in NVS/key-value storage.
3. Host epoch as the last fallback.

The persistent epoch is written when:

- time is set through `POST /api/time`;
- time is set over WebRTC/MCP;
- SNTP synchronises the clock;
- the server epoch differs by more than 60 seconds;
- the firmware periodically checkpoints the current clock (every 10 minutes).

The public server includes `time` (Unix epoch seconds) in every CoAP poll
response. The controller applies it when the drift exceeds two seconds and
persists it when the drift exceeds 60 seconds. Without RTC or battery backup,
the restored value can be stale by the length of the power-off interval; the
first CoAP poll after boot corrects it.

## Live ESP32 checks already completed

These checks were completed on the firmware image that was flashed before the
persistent-time build:

| Check | Result |
|---|---|
| `/health` | `{"ok":true,"device":"mraf","fw":"1.1.0","simulator":false}` |
| Status auth | 401 without credentials, 200 with credentials |
| Dashboard served | HTTP 200 |
| Schedule relay 2 `16:59→05:30` ON | PASS |
| Schedule relay 2 `05:30` OFF | PASS |
| Manual relay off/on/auto via HTTP and MCP | PASS |
| Relay 1-12 GPIO output readback | PASS after GPIO input-enable fix |
| Self-test | 20/22 PASS; battery ADC and ACS712 current remain FAIL |
| Pump interlocks | Tank FULL and sump DRY correctly block AUTO |
| Power gate | `safety.assume_mains_present=true` allows schedule/manual control on USB power |
| MCP tools/list/get_status/control/chat/WireGuard | PASS |
| Wi-Fi scan | HTTP 200 |
| Config round-trip and event log | PASS |
| 23-check live HTTP/MCP E2E script | 23/23 PASS |

## Public server checks completed

| Check | Result |
|---|---|
| `/api/health` | PASS, `service=mraf-springboot`, `devices=1` |
| `/api/me` | PASS with admin credentials |
| `/api/devices` | Device `ESP32-B03FD3` present and confirmed |
| `/api/devices/{id}` | PASS |
| `/api/state` | PASS |
| `/api/devices/{id}/data-request` | PASS |
| `/api/devices/{id}/control` | Queued commands accepted |
| `/api/devices/{id}/config` | Available |
| `/api/devices/{id}/console` | Available |
| `/api/devices/{id}/queue` | Queue clear PASS after transaction fix |
| `/api/devices/{id}/confirm`, `/reject`, `/update` | Available |
| `/mcp` JSON-RPC `initialize`, `tools/list`, `tools/call` | PASS |
| Dashboard served by server | PASS |

Server deployment:

- Previous jar backed up to `/opt/mraf/mraf-server.jar.bak`.
- New jar installed at `/opt/mraf/mraf-server.jar`.
- `mraf.service` active and public `/api/health` returned 200 after startup.
- Stale queued requests for `ESP32-B03FD3` were cleared.

## Production remote-link finding

During the first live server E2E, the device received an oversized telemetry
request from the server. The old firmware had a 640-byte CoAP packet buffer and
could not build the request, returning `CoAP error -1`/`-2`; the server then
showed the device offline with `inflight=true`. This is fixed in the new image
by increasing the CoAP request/response buffers to 4096/8192 bytes.

The live device is still running the older flashed image, so the remote link is
expected to remain unstable until the new `zephyr.bin` is flashed. After
flashing, restore the remote config and rerun the post-flash checks below.

## Post-flash production checklist

After flashing artifact SHA
`173a85df8db2467c706ed3dfa02f55c42ffc8706fd82e7c750be636689c6cc97`:

1. Confirm `/api/status` reports `time_valid=true` and a plausible `now_local`.
2. Power-cycle the controller and confirm it boots with the retained time
   instead of `1970-01-01`.
3. Apply remote configuration:

   ```bash
   curl -u <dashboard-user> -H 'Content-Type: application/json' \
     -X POST http://192.168.0.201/api/config \
     -d '{"wifi":{"hostname":"mraf"},"coap":{"enabled":true,"host":"152.67.176.110","port":5683,"path":"/mraf","interval_sec":60,"user":"farm","pass":"<device-password>","token":"<MRAF_TOKEN>","register":true},"webrtc":{"server":"https://farm.ritebussiness.com"}}'
   ```

4. Confirm device status shows:

   - `remote.enabled=true`
    - `remote.host=152.67.176.110`
   - `remote.status=confirmed`
   - `remote.last_ok=true`
   - `remote.last_code` in the 2.xx CoAP range

5. Confirm server `/api/health` shows `controllerOnline=true` and the device
   has a fresh `lastPoll`.
6. Queue a safe relay command from the server and confirm the device applies
   and acknowledges it.
7. Restore all relays to `auto`.

## Post-flash E2E run (2026-11-21, 19:00 IST)

The user flashed the persistent-time image. Results:

| Check | Result | Evidence |
|---|---|---|
| Firmware boots | PASS | `/health` returns `ok=true`, `device=mraf`, `fw=1.1.0`, `simulator=false` |
| Time survived flash | PASS | Boot time `2026-11-21 18:54:14`, `time_valid=true` |
| Time survived `/api/reboot` | PASS | Uptime `59s`, time `18:59:59`, `time_valid=true` |
| Server epoch correction | PASS | After first successful poll, time became `19:10:41` |
| USB-power gating | PASS after config | `assume_mains_present` reset to false on reflash; set to true; `power.present=true`, `power.assumed=true` |
| Yard Light schedule | PASS | `16:58:58 OFF`, `16:59:09 ON`, `05:30:03 OFF`, restored `ON` in AUTO |
| Dashboard/MCP relay control | PASS | R2 OFF → ON → AUTO via MCP applied |
| Full live HTTP/MCP E2E | 23/23 PASS | Includes dashboard, auth, config, events, Wi-Fi, WireGuard, chat, MCP, pump fault reset |
| Self-test | 20/22 PASS | Battery ADC and ACS712 current still FAIL |
| Public server health | PASS | `/api/health` returned `controllerOnline=true` after successful poll |
| Server queue clear | PASS | `DELETE /api/devices/ESP32-B03FD3/queue` returned `{"ok":true}` |
| Server data request | FAIL | Device reverted to `CoAP error -5`; server `lastData=null` |
| Server control request | FAIL | Commands queued but not acknowledged because polling stopped |
| Remote CoAP link | UNSTABLE | One `last_code=68` after reboot, then repeated `-5` |
| Wi-Fi scan | FAIL | `networks=[]`, `error="scan timed out with no results (radio did not report)"` |
| MCP `tools/list` schema | FAIL | `control_relay` still omits argument properties |

Remote diagnosis:

- Error `-5` is emitted when `zsock_send()` fails on the ESP32.
- Setting `coap.host` to the public IP, the DNS name, and `127.0.0.1` all still produced `-5`, so this is local to the device socket send path.
- A 85-second `tcpdump` on the server saw `0 packets` on UDP `5683`.
- Clearing the server queue did not clear the device-side `pending` state or recover the link.
- Each reboot produced exactly one successful poll (`last_code=68`, server `pollCount` incremented, `controllerOnline=true`), followed by `-5` once an inflight data request was present.
- Most likely causes are local UDP buffer/memory exhaustion or an oversized data-request datagram. Next firmware should capture `errno` from `zsock_send()`, clear the pending data flag after a failed send, and either enable IP fragmentation or reduce the remote payload.

Final state left on the controller:

- Time `2026-11-21 19:30:00`, valid.
- `assume_mains_present=true`.
- Yard Light mode `on` and output `true`.
- Server queue `0`, inflight `false`.
- Remote enabled and registered, but currently `CoAP error -5`.

## MCP and CoAP socket fix build (2026-11-21)

The flashed image exposed two separate defects:

- Device MCP `tools/list` omitted `control_relay` arguments. The device helper
  called ArduinoJson `to<JsonObject>()` / `to<JsonArray>()` for every added
  parameter, which cleared the existing `properties` and `required` arrays. It
  now uses `as<JsonObject>()` / `as<JsonArray>()` and preserves earlier fields.
- Remote CoAP returned `CoAP error -5` before any packet reached the server. The
  new firmware logs `errno`, retries `zsock_send()` with socket recreation,
  clears pending data after repeated failures, drops the unused `equipment`
  array from remote telemetry and raises Zephyr packet/buffer counts.

Validation after the fix:

| Check | Result |
|---|---|
| Spring Boot server tests | PASS |
| C++ schedule/controller tests | PASS |
| Python simulator tests | 53/53 PASS |
| Zephyr native_sim E2E | 18/18 PASS |
| ESP32 firmware build | PASS |
| Firmware SHA256 | `173a85df8db2467c706ed3dfa02f55c42ffc8706fd82e7c750be636689c6cc97` |

After flashing this build, use the IP address as `coap.host` to avoid DNS
ambiguity during verification:

```bash
python3 /tmp/opencode/post_fix_e2e.py
```

The script applies the IP CoAP config, verifies the MCP schema, clears the
server queue, issues a safe `data-request`, issues a safe relay `control`
request, restores relay 2 to `auto`, and prints a pass/fail summary.

## Post-reflash verification (2026-11-21, 20:11 IST)

The production controller was reflashed with SHA
`173a85df8db2467c706ed3dfa02f55c42ffc8706fd82e7c750be636689c6cc97` and rechecked
with `coap.host=152.67.176.110`.

| Check | Result | Evidence |
|---|---|---|
| IP CoAP config | PASS | `remote.host=152.67.176.110`, `enabled=true` |
| Persistent time | PASS | `2026-11-21 20:11:26`, `time_valid=true` |
| MCP `control_relay` schema | PASS | properties `mode`, `pulse_ms`, `relay_id`; required `mode`, `relay_id` |
| CoAP polling | PASS | `last_code=68`, `last_ok=true`, `last_error=""`, `status=confirmed` |
| Server data request | PASS | `data-19` acked, `lastData` populated |
| Server control request | PASS | `ctl-20` acked with `result=applied` |
| Relay restoration | PASS | Yard Light mode `auto`, output ON by schedule |
| Server queue state | PASS | `online=true`, `queued=0`, `inflight=false` |
| Public server health | PASS | `controllerOnline=true`, `devices=1` |
| Full live HTTP/MCP E2E | 23/23 PASS | No regressions after reflash |
| IP-based focused E2E | 9/9 PASS | `/tmp/opencode/post_fix_e2e.py` |

## Motor contactor START/STOP GPIO pulse test

A live-only test is available for the latching motor/contactor output on relay 2.
It temporarily configures channel 2 as a motor pulse output, runs two START/STOP
cycles, verifies the active GPIO for each pulse, then restores the previous
channel config and sets the relay back to `auto`.

By default START is GPIO14 and STOP is GPIO15, with 1500 ms pulses. Override the
wiring if needed:

```bash
export MRAF_AUTH_USER=<controller-user>
export MRAF_AUTH_PASS=<controller-password>
MRAF_ON_PIN=14 MRAF_OFF_PIN=15 MRAF_PULSE_ON_MS=1500 MRAF_PULSE_OFF_MS=1500 \
  make gpio14-pulse-test
```

The script is `tests/gpio14_motor_pulse_test.py`. It does not touch the 5 HP
pump contactor on GPIO33. The dashboard now reads `pin`, `pin_off`, and
`output_pin` from `/api/status`, shows separate START/STOP GPIOs for motor
contactors, uses separate START/STOP pulse duration inputs, and refreshes the
active profile from `/api/config` on load. `buildConfig()` removes the local
`profiles` cache before POSTing `/api/config` so settings saves do not exceed
the firmware request body limit.

## Separate START/STOP GPIO build

Firmware SHA256 `7061566a2a99bbf002051dad6b5c6daa8b56e66d48130be6c99a703a2735c9e2`
is reflashed and validated on the live controller. START is GPIO14 and STOP is
confirmed as GPIO15.

| Check | Result | Evidence |
|---|---|---|
| Dashboard JavaScript syntax | PASS | `node --check` |
| C++ controller and schedule tests | PASS | `make test` |
| Simulator API tests | 53/53 PASS | `make test` |
| Zephyr build (native and ESP32) | PASS | `make zephyr-build` |
| Zephyr native E2E | 18/18 PASS | `make zephyr-e2e` |
| Settings POST excludes local `profiles` cache | PASS | E2E dashboard assertion |
| Live full HTTP/MCP E2E | 23/23 PASS | `/tmp/opencode/e2e_live.py` |
| Live focused CoAP/MCP/IP E2E | 9/9 PASS | `data-28`, `ctl-29`, `last_code=68` |
| Live separate-GPIO pulse verification | 12/12 PASS | GPIO14 START, GPIO15 STOP, 2 cycles |
| Live settings save verification | PASS | No HTTP 413 |
| Live `/api/status` after events | PASS | Valid JSON, 16 KiB buffer with truncation guard |

### Final live hardware state

| Item | Result |
|---|---|
| Device time | Valid, 2026-11-21 21:35 IST |
| Yard Light (R2) | `mode=auto`, `on=true`, `pin=14`, `pin_off=14`, schedule active |
| Pump | `mode=auto`, `on=false`, `state=EXTERNAL` |
| Remote CoAP | Enabled, `152.67.176.110`, `last_ok=true`, `status=confirmed` |
| Public server | `controllerOnline=true`, `devices=1` |
| Self-test | 20/22; battery ADC and ACS712 checks are known non-blocking failures |

## Known non-blocking issues

| Issue | Status |
|---|---|
| Battery ADC reads about `0.77 V` with USB-only supply | Self-test FAIL; expected without battery |
| ACS712 reads about `33 A` with pump off | Self-test FAIL; pump state becomes `EXTERNAL` |
| Wi-Fi scan returns no networks | `networks=[]`, `error="scan timed out with no results (radio did not report)"`; HTTP 200 and non-blocking |
| Settings save returns HTTP 413 `request body too large` | Local `profiles` cache removed from `/api/config` POST body; fixed and verified live in `7061566a...` |
| `/api/status` truncated after event log grows | HTTP response buffer raised to 16 KiB with truncation guard; fixed and verified live in `7061566a...` |
| MCP `tools/list` schema for `control_relay` omitted argument properties | Fixed and verified after reflash with SHA `173a85df...` |
| Remote CoAP send returned `-5` after a data request | Fixed and verified after reflash with SHA `173a85df...` |
| No RTC coin cell / battery backup | Persistent epoch gives approximate time at boot; server CoAP time corrects it |
| No OTA path in firmware | ESP32 must be flashed manually |
