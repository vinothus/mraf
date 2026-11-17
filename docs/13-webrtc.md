# 13 - WebRTC peer-to-peer dashboard link

The device dashboard normally talks to the controller over plain HTTP on the
LAN. When the controller and the browser are not on the same network - for
example the browser is on the operator's phone over the internet - that HTTP
traffic has to be tunnelled. WebRTC solves this without routing the live data
through the server: the browser and the controller open a **direct, encrypted
data channel** and the dashboard reads and writes over that channel instead.

The Spring Boot server only helps the two peers find each other. It:

- runs a small **STUN server** so ICE can discover the public address of each
  peer,
- advertises a **TURN relay** (coturn or any RFC 5766 server) so a link still
  works when a direct/STUN path is blocked, and
- relays **SDP offers/answers** through a per-device signalling registry.

The controller is an ICE-lite answerer: it advertises a host candidate, an
optional server-reflexive (STUN) candidate and, when TURN credentials are
configured, a relayed candidate. The browser's ICE agent picks the best working
pair. If the STUN lookup fails (dead or blocked STUN server, symmetric NAT,
QEMU slirp) the device keeps the relay candidate and the data channel falls
back to TURN, so the dashboard still works. `/api/status.webrtc` (and the Live
P2P card) reports the selected `path` as `direct` or `relay`.

Once the data channel is open the server never sees the dashboard traffic - it
is DTLS-encrypted and flows directly between the browser and the controller. It
only receives a short **status report** (state, RTT, byte counters) so the admin
"Live links" tab can show what is connected.

```
browser ──offer/answer (SDP)──► server signalling registry ◄──offer/answer── ESP32 / simulator
   │                                                                              │
   └──────────────── DTLS/SCTP data channel (direct, P2P) ───────────────────────┘
        (server sees nothing here)
```

## Roles

The **browser is always the offerer**: it creates the data channel, builds the
SDP offer and posts it. The **controller is the answerer**: it polls for the
offer, hands it to its peer stack, and posts the answer SDP. This keeps the
device simple - it never has to decide when to call someone.

## Server control plane

Endpoints (all under `/api/webrtc`, JSON):

| Method | Path | Auth | Purpose |
|---|---|---|---|
| `GET` | `/config?device_id=…` | token | STUN URL, ICE servers, signalling URLs, poll interval |
| `POST` | `/signal` | token | Post an SDP offer/answer/candidate (`from` = `browser`\|`device`) |
| `GET` | `/signal?device_id=…&role=…&since=…` | token | Poll for messages addressed to `role` with `seq > since`; also returns the mailbox `seq` head |
| `POST` | `/report` | token | Peer status report (state, RTT, bytes) |
| `GET` | `/sessions` | admin | Live session snapshot for the dashboard |
| `DELETE` | `/sessions/{id}` | admin | Drop a session |

The shared token is `mraf.token` (default `demo-token`) sent in the
`X-Mraf-Token` header. `/api/webrtc/**` is `permitAll` in Spring Security; the
token is enforced inside `WebRtcController`. `/sessions` additionally requires
`ROLE_ADMIN`.

The registry (`WebRtcRegistry`) keeps one session per `device_id` with:

- an append-only list of signalling messages, each with a monotonic `seq`, the
  `from`/`to` role and the `type` (`offer`, `answer`, `candidate`, `bye`),
- the latest report from each role, and
- a TTL janitor (`mraf.webrtc.signal.ttl-seconds`, default 180 s).

A poller must pass the `seq` of the last message it processed as `since`; the
registry returns only newer messages addressed to its role, plus the current
mailbox head in the `seq` field of the response.

Two rules keep reconnects safe:

- **A restarting device adopts the head as its initial cursor.** The device
  discards the `since` cursor on startup, does one poll to read the head `seq`,
  and starts from there. Without this it would replay the previous handshake and
  answer a stale offer, producing an SDP whose ICE `ufrag`/`pwd` no longer match
  the live browser (ICE checks then fail with `401 Unauthenticated`).
- **A fresh browser offer clears the mailbox.** When the browser posts a new
  `offer`, the registry drops the previous offer/answer/candidate traffic so a
  stale answer can never be handed back to the browser. Sequence numbers stay
  monotonic.

The browser also baselines its own `since` to the `seq` returned when it posts
its offer, so it ignores anything from before this negotiation.

### Configuration

```yaml
mraf:
  webrtc:
    enabled: ${MRAF_WEBRTC_ENABLED:true}
    public-host: ${MRAF_WEBRTC_HOST:}      # host advertised in the STUN/TURN URLs
    public-url: ${MRAF_WEBRTC_URL:}
    stun:
      enabled: ${MRAF_STUN_ENABLED:true}
      port: ${MRAF_STUN_PORT:3478}
    turn:
      enabled: ${MRAF_TURN_ENABLED:false}
      host: ${MRAF_TURN_HOST:}             # empty: derive from public-host/request
      port: ${MRAF_TURN_PORT:3478}
      url: ${MRAF_TURN_URL:}               # optional full turn: URL
      username: ${MRAF_TURN_USER:}
      credential: ${MRAF_TURN_PASS:}
    signal:
      ttl-seconds: ${MRAF_WEBRTC_TTL:180}
      max-queue: ${MRAF_WEBRTC_QUEUE:256}
      poll-ms: ${MRAF_WEBRTC_POLL_MS:400}
      max-payload-bytes: ${MRAF_WEBRTC_MAX_PAYLOAD:16384}
```

Open UDP `3478` (STUN **and** TURN) and TCP `8080` (signalling) to the
browsers. Set `MRAF_WEBRTC_HOST` to the hostname/IP that clients can reach when
the server is behind NAT or a load balancer. When `MRAF_TURN_HOST` is empty the
config endpoint derives it from `MRAF_WEBRTC_HOST` or the requesting host, so
the device learns the same address the browser used. TURN credentials are
static (`lt-cred-mech`); the server advertises them to both peers. Disable the
relay advertisement with `MRAF_TURN_ENABLED=false` if no TURN server is
deployed.

## Browser flow

1. Load the device dashboard and read `GET /api/webrtc` from the **device**
   (not the server). That returns `enabled`, `server` (control-plane URL),
   `stun`, `turn`, `turn_user`, `poll_ms` and the shared `token`.
2. If enabled, `GET <server>/api/webrtc/config` for the ICE server list
   (`stun:` and `turn:` URLs plus TURN credentials). Empty device settings are
   filled from this response; explicit device values win.
3. Create the `RTCPeerConnection` with that `iceServers` list. The Live P2P tab
   has a **force relay** toggle that sets `iceTransportPolicy: "relay"` so only
   the TURN path is considered (useful to prove the fallback).
4. Add a data channel, `createOffer()`, `setLocalDescription()`, then
   `POST /api/webrtc/signal` with the offer.
5. Remember the returned `seq` and poll `/signal?role=browser&since=<seq>` for
   the answer; `setRemoteDescription({type:"answer", sdp})`.
6. On `datachannel` open, route the dashboard's `/api/status`, `/api/config`,
   `/api/events`, `/api/control`, `/api/chat`, `/api/time`, `/api/wireguard`
   and `/api/pump/reset` calls through the channel (HTTP fallback otherwise).
7. Periodically `POST /api/webrtc/report` so the admin tab can show the link.

The channel protocol is request/reply:

```json
// request
{"id": 7, "cmd": "control", "body": {"target": "relay", "id": 1, "mode": "on"}}
// reply
{"id": 7, "ok": true, "data": {"ok": true, "relays": [ ... ]}}
```

Commands: `ping`, `status`, `config`, `events`, `wireguard`, `control`, `chat`,
`time`, `pump-reset`, `bye`. Replies always echo `id`; failures carry `error`.

## Device / firmware flow

The firmware module lives in `firmware/zephyr/src/webrtc.cpp` with the public
API in `include/webrtc.h`. It is the answerer:

1. Poll `GET /api/webrtc/signal?device_id=…&role=device&since=…`. On the first
   poll after boot the module adopts the returned head `seq` as its cursor
   (skipping any stale history) before processing offers.
2. For a `offer`, call `mraf_platform_webrtc_start(offer, answer, cap)`, which
   does the STUN lookup, allocates a TURN relay when configured (even if STUN
   failed) and returns the answer SDP with host/srflx/relay candidates already
   gathered into it.
3. `POST /api/webrtc/signal` with `from:"device", type:"answer"`.
4. Dispatch inbound data-channel messages through the same controller helpers
   the HTTP API uses (`mraf_apply_control`, `settingsFromJson`,
   `mraf_intent_parse/apply`, `mraf_build_status`).
5. `POST /api/webrtc/report` every couple of seconds while connected.

A new `offer` always tears down any previous peer connection first, so a browser
reconnect (or a device that was still holding an old link) starts from a clean
DTLS/ICE state instead of renegotiating a half-open channel. Teardown sends a
TURN `Refresh(0)` so the next connection can reallocate the same 5-tuple
immediately.

Only the **transport** is platform specific. `webrtc.cpp` is transport
agnostic: the signalling HTTP client and the command dispatcher are shared by
every build. The transport boundary is:

```c
bool  mraf_platform_webrtc_available(void);
bool  mraf_platform_webrtc_start(const char *offer_sdp, char *answer_sdp, size_t cap);
void  mraf_platform_webrtc_stop(void);
bool  mraf_platform_webrtc_open(void);
int   mraf_platform_webrtc_send(const char *json);
char *mraf_platform_webrtc_tx_begin(size_t *cap);
int   mraf_platform_webrtc_tx_end(size_t len);
void  mraf_platform_webrtc_set_rx(mraf_webrtc_rx_cb cb);
```

When `CONFIG_MRAF_WEBRTC=y`, both `platform_native.cpp` and
`platform_esp32.cpp` delegate to the **same portable peer** in
`firmware/zephyr/src/webrtc_peer.cpp` (Zephyr zsock + mbedTLS only), so the
code tested against Chrome on native_sim is the code that runs on the ESP32:

- ICE-lite answerer: STUN binding responder with MESSAGE-INTEGRITY and
  FINGERPRINT, a host candidate, an optional srflx candidate from the
  configured STUN server, and an optional relay candidate from a TURN
  allocation on the same UDP socket.
- TURN (RFC 5766) client: Allocate/Refresh/CreatePermission, long-term
  credentials (MD5 key derived locally), `Send`/`Data` Indications and
  XOR-PEER-ADDRESS parsing. Incoming relayed DTLS/STUN is demultiplexed from
  the same socket; outgoing payloads use `Send` Indications while the relay
  path is selected. Allocation recovery handles `437 Allocation Mismatch`
  (bootstrap a fresh realm/nonce, `Refresh(0)`, retry).
- DTLS 1.2 server (`a=setup:passive`) using the embedded P-256 certificate,
  including the `HELLO_VERIFY_REQUIRED` cookie exchange (datagram packing off).
- Minimal SCTP association: INIT/INIT-ACK, SACK, HEARTBEAT, SHUTDOWN/ABORT,
  CRC32c (little-endian), reassembly, and one DCEP-negotiated stream (`mraf`,
  id 0). SCTP chunks are zero-padded to a 4-byte boundary before the CRC is
  computed — Chrome's usrsctp silently drops unpadded chunks.
- Replies are serialized straight into the peer's tx buffer
  (`tx_begin`/`tx_end`) and fragmented into DATA chunks; inbound messages use
  a separate fixed 4 KiB reassembly buffer.

Without `CONFIG_MRAF_WEBRTC` the module stays idle (`available() == false`)
and the dashboard falls back to HTTP.

### Firmware configuration

The `webrtc` block in `/api/config`:

```json
{
  "webrtc": {
    "enabled": true,
    "server": "http://10.0.0.5:8085",
    "stun": "",
    "turn": "",
    "turn_user": "",
    "turn_pass": "",
    "poll_ms": 400
  }
}
```

`server` is required when `enabled` is true. `stun`, `turn`, `turn_user` and
`turn_pass` are optional and are filled from the server's
`GET /api/webrtc/config` when empty, so a device normally only needs the
control-plane URL. Explicit values survive a config fetch, which lets a test
rig point the device at a dead STUN port while keeping the server's TURN
settings. `turn` accepts `turn:host:port` or `host:port`; `poll_ms` must be
`100..5000`. When TURN credentials are present the device always advertises a
relay candidate, so the link survives a failed STUN lookup.

Kconfig options (`firmware/zephyr/Kconfig`):

| Option | Default | Meaning |
|---|---|---|
| `CONFIG_MRAF_WEBRTC` | `n` | Link a DTLS/SCTP transport (off: module inert) |
| `CONFIG_MRAF_WEBRTC_SDP_MAX` | `4096` | Max SDP answer size; `webrtc.conf` sets 1024 |
| `CONFIG_MRAF_WEBRTC_MSG_MAX` | `2048` | Max outbound data-channel message size; `webrtc.conf` sets 8192 |

`webrtc.conf` lowers `SDP_MAX` because the controller only *builds* the answer
(~600 B); the browser's offer is parsed straight out of the signalling HTTP
response buffer. `MSG_MAX` is raised because a `status` reply is ~5.7 kB.

### Verifying STUN, TURN and ICE on qemu_x86

The QEMU build uses slirp user networking. The guest's host candidate
(`10.0.2.15:5000`) is not reachable from the host, and guest UDP leaves through
the slirp NAT, so the direct path only works when the browser can reach the
device's srflx mapping. That makes QEMU a realistic relay test: with a dead
STUN server the link must fall back to TURN.

Bring up the stack (device HTTP `18083`, server `18085`, STUN/TURN UDP `3478`):

```bash
# coturn: relay sockets stay on loopback; allow loopback peers for the test rig
mkdir -p /tmp/opencode/turn
cat > /tmp/opencode/turn/turnserver.conf <<'EOF'
listening-port=3478
listening-ip=0.0.0.0
relay-ip=127.0.0.1
external-ip=127.0.0.1
fingerprint
lt-cred-mech
user=mraf:mraf-turn-secret
realm=mraf.local
allow-loopback-peers
no-cli
log-file=stdout
verbose
EOF
setsid nohup turnserver -c /tmp/opencode/turn/turnserver.conf \
  > /tmp/opencode/turn/turn.log 2>&1 < /dev/null &

# Test-rig only: coturn's relay socket binds 127.0.0.1, but ICE checks target
# the guest's 10.0.2.15 host candidate. Without this local route sendto() fails
# with EINVAL and coturn closes the whole relay socket.
ip route add local 10.0.2.15/32 dev lo
```

Start the Spring Boot control plane with TURN advertisement and boot QEMU:

```bash
export PATH=/opt/maven/bin:$PATH
# Leave MRAF_WEBRTC_HOST / MRAF_TURN_HOST empty: the config endpoint derives
# the advertised host per request, so the browser gets 127.0.0.1 while the
# QEMU guest gets 10.0.2.2. Pinning one global host breaks the other side.
MRAF_TURN_ENABLED=true MRAF_TURN_USER=mraf MRAF_TURN_PASS=mraf-turn-secret \
  java -jar server/target/mraf-server-1.0.0.jar &

west build -s firmware/zephyr -b qemu_x86 -d firmware/zephyr/build/qemu_x86 -- \
  -DEXTRA_CONF_FILE="qemu-x86.conf;webrtc.conf"
west build -d firmware/zephyr/build/qemu_x86 -t run &   # device HTTP -> :18083

# Point the controller at the control plane (guest reaches the host at 10.0.2.2).
curl -s -u admin:farm1234 -H 'Content-Type: application/json' \
  -d '{"coap":{"token":"demo-token"},"webrtc":{"enabled":true,
       "server":"http://10.0.2.2:18085","stun":"","turn":"",
       "turn_user":"","poll_ms":400}}' \
  http://127.0.0.1:18083/api/config
```

Saving the WebRTC block makes the controller re-read
`GET /api/webrtc/config`, so empty `stun`/`turn`/`turn_user` fields are filled
from the server without a reboot. Explicit values survive that fetch, which is
how the dead-STUN scenario below is set up.

The `ice` object from the device's `GET /api/webrtc` (also in
`/api/status.webrtc`) shows what it gathered:

```json
{"stun_ok": true, "srflx": "127.0.0.1:55519",
 "relay": "127.0.0.1:50168", "relay_allocated": true, "path": "direct"}
```

Three checks (Chromium is driven by Playwright; `force` sets
`iceTransportPolicy: "relay"`):

| Scenario | How | Expected |
|---|---|---|
| Direct / STUN | live STUN + TURN; default policy | `path: direct`, `Data source: WebRTC P2P` |
| Relay fallback | point `webrtc.stun` at a dead port (e.g. `stun:10.0.2.2:3999`), keep TURN | `stun_ok: false`, `relay_allocated: true`, `path: relay`, `relay (TURN)` |
| Relay only | dead STUN + force-relay browser policy | browser candidate list contains only `relay`; ICE pair `relay ↔ relay`; dashboard connects |

`POST /api/config` with an explicit `webrtc.stun` keeps that value even when
the server later advertises a live STUN URL, which is how the dead-STUN
fallback is exercised. The Live P2P card reports `TURN allocation`,
`Relay address` and `Selected path`; `/api/status.webrtc` carries the same
fields for automation.

The recorded proof is at
[docs/demo/mraf-qemu-turn-fallback.mp4](demo/mraf-qemu-turn-fallback.mp4): the
QEMU image comes up on the direct STUN path, is repointed at a dead STUN port
and reconnects through TURN, then `Force TURN relay` restricts the browser to
relay candidates so the selected pair is `relay ↔ relay`. Reproduce it with
`python3 scripts/record_qemu_turn_fallback_demo.py` after bringing up the rig
above.

### ESP32-WROOM memory budget (no PSRAM)

`CONFIG_MRAF_WEBRTC=y` now links and fits the 192 KiB `dram0_0_seg` with the
TURN/ICE additions:

| Build | DRAM used | Free |
|---|---|---|
| `esp32_devkitc/esp32/procpu` (no WebRTC) | 160,088 B (81.4%) | 36,520 B |
| same + `EXTRA_CONF_FILE=webrtc.conf` (host/STUN only) | 192,280 B (97.8%) | 4,328 B |
| same + TURN/ICE fallback (current) | 195,272 B (99.32%) | 1,336 B |
| Application flash | 950,368 B / 4,194,048 B | 22.66% used |

The transport adds ~35 KiB of static RAM on top of the trimmed baseline:
`g_peer` (8 KiB tx + 4 KiB rx + TURN/SCTP/DTLS state) 14,616 B, peer thread
stack 12,288 B, signalling thread stack 6,144 B, one 5,120 B HTTP exchange
buffer (request and response bodies are compacted in place), a 4,096 B request
mailbox, a 4,096 B signalling scratch buffer and ~3 KiB for the SDP
answer/post/reply state. AES tables are forced into flash so they no longer
cost 8 KiB of DRAM.

Everything else was trimmed to match the real payload sizes, not arbitrary
round numbers:

- system heap 36,864 B, main/HTTP-server stacks 6,144 B
- HTTP response 8,192 B, request body 4,096 B, settings JSON 4,096 B
- remote poll body/response 3,072/5,120 B, remote stack 4,096 B
- CoAP receive 3,072 B, MCP tool text 2,048 B, time-sync stack 3,072 B
- data-channel reply is serialized **directly** into the peer tx buffer via
  `mraf_peer_tx_begin()`/`mraf_peer_tx_end()`, so no second ~8 KiB reply buffer
- peer rx is fixed at 4,096 B (largest inbound message is a config save), while
  tx follows `CONFIG_MRAF_WEBRTC_MSG_MAX`

Rules of thumb when touching this code: keep `WRT_HTTP_BUF` above the largest
signalling JSON (offer + envelope), keep the request mailbox above the largest
DC request (config save ≈ 3.2 KiB), keep `MSG_MAX` above the largest reply
(status ≈ 5.7 KiB), and don't add static buffers without re-checking
`dram0_0_seg`. The 1.3 KiB DRAM margin is now the design gate; runtime heap
usage of the DTLS handshake plus status/config documents must stay below the
36,864 B pool (validated on native_sim and qemu_x86, still to be confirmed on
hardware).

## Simulator

`simulator/webrtc_peer.py` is a complete aiortc answerer used for development
and demos. Enable it with:

```bash
python3 simulator/mraf_sim.py \
  --webrtc --webrtc-server http://localhost:8085 --token demo-token
```

The simulator also serves `GET /api/webrtc` (settings) and
`GET /api/webrtc/status` for the dashboard. The `--token` flag sets the shared
signalling token (`coap.token`).

## Demo recording

[demo/mraf-esp32-webrtc-ui-demo.mp4](demo/mraf-esp32-webrtc-ui-demo.mp4) shows
the device UI with the transport enabled, not the server dashboard. It is the
`native_sim` build of the ESP32 firmware, so the HTML/JS in `webui.h`, the REST
handlers and `webrtc_peer.cpp` are the same code that runs on the ESP32. The
recording covers the HTTP-served dashboard, relay control, daily and recurring
schedules, the WebRTC settings and Live P2P tab, WireGuard state, relay and
pump commands over the data channel, offline grammar chat, and the event log.
Regenerate it with `scripts/record_esp32_webrtc_ui_demo.py` while the
native-webrtc firmware and Spring Boot control plane are running.

To check that a WebRTC ESP32 image still contains the local UI and API, inspect
the ELF:

```bash
nm firmware/zephyr/build/esp32-webrtc/zephyr/zephyr.elf |
  grep -E 'http_server_start|mraf_api_start|mraf_webrtc_start|mraf_peer_start'
```

`webui.h` is generated into `firmware/zephyr/build/esp32-webrtc/webui.h` and the
only difference from the default ESP32 `.config` is the `CONFIG_MRAF_WEBRTC*`
block, so enabling the transport does not remove any other feature.

## Gotchas

- **SCTP padding.** Every SCTP chunk must be padded to a 4-byte boundary inside
  the packet (the chunk length field still excludes the padding); CRC32c covers
  the padded datagram. aiortc tolerates unpadded chunks, Chrome does not.
- **DTLS cookies.** A server-mode mbedTLS handshake can return
  `MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED`; reset the session, re-apply the
  client transport id/timer, disable datagram packing and retry.
- **PSA options.** DTLS 1.2 PRF/HMAC needs `CONFIG_PSA_WANT_ALG_TLS12_PRF`,
  `CONFIG_PSA_WANT_ALG_HMAC` and `CONFIG_PSA_WANT_KEY_TYPE_DERIVE`, otherwise
  the handshake fails with `-0x7f80`.
- **Chromium mDNS.** By default Chromium hides local IPs behind mDNS
  (`WebRtcHideLocalIpsWithMdns`), so host candidates become `…​.local` names that
  aiortc cannot resolve, and the only other candidate is loopback - ICE fails.
  For local demos launch Chromium with
  `--disable-features=WebRtcHideLocalIpsWithMdns`. On a real LAN the STUN
  server provides a routable `srflx` candidate, so no flag is needed.
- **Full SDP, not trickle.** aiortc (1.16) emits no `icecandidate` event;
  candidates only appear in `localDescription.sdp` once ICE gathering completes.
  Both peers therefore wait for `icegatheringState == "complete"` and exchange a
  single full SDP.
- **Mailbox direction.** `poll(deviceId, role, since)` must return messages
  addressed **to** `role`, not to the other side. Getting this backwards makes
  both peers poll each other's messages and the link never connects.
- **Baseline the `since` value.** After posting the offer, set the browser's
  `since` to the `seq` the server returned, otherwise stale answers from a
  previous session are replayed.
- **TURN Send Indication length.** The STUN message length covers only the
  attributes (`XOR-PEER-ADDRESS` 12 B + `DATA` 4 B + padded payload); adding a
  second 4-byte header to the length makes coturn drop every relayed packet
  without logging an error.
- **XOR address wire format.** STUN/TURN XOR addresses are
  `[reserved=0, family=1, xor-port, xor-ip]` for IPv4. Swapping family and
  reserved makes coturn answer `403 Forbidden IP`.
- **DTLS send callback return.** mbedTLS compares the BIO send return with the
  record length it asked to send. A relayed datagram is wrapped in a TURN
  Send Indication, so `sendmsg()` reports header+payload bytes; returning that
  makes mbedTLS fail with `-0x6c00`. Always return the DTLS `len`.
- **TURN allocation mismatch (`437`).** coturn answers `437` before
  authentication when the 5-tuple still owns an allocation (fast reconnect or
  reboot). Probe from a throwaway socket to learn realm/nonce, send
  `Refresh(0)`, then retry Allocate. Teardown also sends `Refresh(0)`.
- **Relay sockets and loopback peers.** Bind the relay to a reachable address
  (loopback for the QEMU rig). If the relay is bound to `127.0.0.1` and a peer
  target is outside loopback, `sendto()` returns `EINVAL`, coturn marks the
  relay `tobeclosed` and the allocation dies. On the QEMU rig add
  `ip route add local 10.0.2.15/32 dev lo` or use a wildcard relay with an
  `external-ip` mapping and `allow-loopback-peers`.
- **Signalling cursor vs. server restarts.** The registry lives in memory, so a
  control-plane restart resets each device's sequence to zero. A device that
  kept a higher cursor would ignore every new offer until reboot, so the poller
  detects `head < since` and re-syncs to the new head. Saving the WebRTC block
  also clears the cached `GET /api/webrtc/config` result, so new STUN/TURN
  settings apply without a reboot.
- **HTTPS/`https://` control URLs** are rejected by the firmware's minimal HTTP
  client (no TLS). Use `http://` on the trusted LAN, or put the signalling
  behind the WireGuard tunnel from [10-remote-coap.md](10-remote-coap.md).
