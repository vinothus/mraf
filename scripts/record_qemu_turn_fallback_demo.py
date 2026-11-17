#!/usr/bin/env python3
"""Record the qemu_x86 build falling back from STUN to TURN.

Runs against the QEMU x86 image of the ESP32-class firmware (:18083) with the
Spring Boot control plane (:18085) and coturn (UDP 3478) on the host.

The recording has three acts:

1. live STUN + TURN  -> the browser selects the direct srflx path
2. dead STUN         -> the controller allocates a TURN relay and the link
                        still comes up (path relay)
3. force relay       -> browser policy ``relay``; the selected pair is
                        relay <-> relay

Prerequisite: boot the image with ``python3 /tmp/opencode/qemu_up.py`` (or
``west build -t run``) and start the server with TURN advertisement, e.g.

    MRAF_TURN_ENABLED=true java -jar server/target/mraf-server-1.0.0.jar

Output: docs/demo/mraf-qemu-turn-fallback.mp4 (H.264, yuv420p, faststart).
"""

import base64
import http.client
import json
import os
import shutil
import subprocess
import sys
import time

from playwright.sync_api import sync_playwright

DEV = os.environ.get("MRAF_DEV_URL", "http://127.0.0.1:18083/")
SERVER = os.environ.get("MRAF_SERVER_URL", "http://127.0.0.1:18085")
USER = os.environ.get("MRAF_DEV_USER", "admin")
PASS = os.environ.get("MRAF_DEV_PASS", "farm1234")
TOKEN = os.environ.get("MRAF_TOKEN", "demo-token")
RAW_DIR = os.environ.get("MRAF_VIDEO_RAW", "/tmp/mraf-qemu-turn/raw")
OUT = os.environ.get("MRAF_VIDEO", "/workspace/mraf/docs/demo/mraf-qemu-turn-fallback.mp4")
SIZE = {"width": 1280, "height": 800}
AUTH = base64.b64encode(f"{USER}:{PASS}".encode()).decode()

LIVE_STUN = "stun:10.0.2.2:3479"
DEAD_STUN = "stun:10.0.2.2:3999"
DEVICE_TURN = "turn:10.0.2.2:3478"


def dev_req(path, method="GET", body=None, timeout=15):
    last = None
    for _ in range(30):
        try:
            c = http.client.HTTPConnection("127.0.0.1", 18083, timeout=timeout)
            data = json.dumps(body).encode() if body is not None else None
            h = {"Authorization": "Basic " + AUTH}
            if data:
                h["Content-Type"] = "application/json"
            c.request(method, path, body=data, headers=h)
            r = c.getresponse()
            return r.status, r.read().decode()
        except OSError as exc:
            last = exc
            time.sleep(0.5)
    raise SystemExit(f"device unreachable: {last}")


def configure_webrtc(stun, turn, wait_for=None):
    body = {
        "coap": {"token": "demo-token"},
        "webrtc": {"enabled": True, "server": "http://10.0.2.2:18085",
                   "stun": stun, "turn": turn, "turn_user": "mraf",
                   "poll_ms": 400},
    }
    dev_req("/api/config", "POST", body)
    for _ in range(60):
        time.sleep(0.5)
        _, raw = dev_req("/api/webrtc")
        info = json.loads(raw)
        if wait_for is None or wait_for(info):
            return info
    raise SystemExit(f"device did not adopt config: {json.dumps(info)}")


OVERLAY_JS = """
() => {
  if (document.querySelector('#demo-caption')) return;
  const el = document.createElement('div');
  el.id = 'demo-caption';
  el.style.cssText = [
    'position:fixed', 'left:0', 'right:0', 'bottom:0', 'z-index:2147483647',
    'padding:22px 34px', 'font:600 21px/1.35 Inter,system-ui,sans-serif',
    'color:#fff', 'background:linear-gradient(90deg,rgba(20,32,50,.94),rgba(20,32,50,.72))',
    'box-shadow:0 -8px 30px rgba(0,0,0,.25)'
  ].join(';');
  document.body.appendChild(el);

  const box = document.createElement('div');
  box.id = 'demo-pair';
  box.style.cssText = [
    'position:fixed', 'right:24px', 'top:18px', 'z-index:2147483647',
    'min-width:430px', 'padding:14px 18px', 'border-radius:12px',
    'font:600 15px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace',
    'color:#d7ffe8', 'background:rgba(12,22,18,.92)',
    'box-shadow:0 10px 30px rgba(0,0,0,.35)'
  ].join(';');
  box.innerHTML = '<div style="font:700 13px/1.4 Inter,system-ui,sans-serif;' +
    'letter-spacing:.12em;color:#7fe0ae">SELECTED ICE PAIR</div>' +
    '<div id="demo-pair-row">negotiating...</div>';
  document.body.appendChild(box);

  const fmt = c => c ? c.candidateType + ' ' + c.address + ':' + c.port : '?';
  setInterval(async () => {
    const row = document.querySelector('#demo-pair-row');
    if (!row) return;
    const pc = (typeof webrtc !== 'undefined') && webrtc.pc;
    if (!pc) { row.textContent = 'no peer connection'; return; }
    try {
      const stats = await pc.getStats();
      const byId = {};
      stats.forEach(s => byId[s.id] = s);
      let best = null;
      stats.forEach(s => {
        if (s.type !== 'candidate-pair') return;
        if (!best && (s.state === 'succeeded' || s.nominated)) best = s;
      });
      if (!best) { row.textContent = pc.iceConnectionState + ' / no pair yet'; return; }
      const l = byId[best.localCandidateId], r = byId[best.remoteCandidateId];
      row.innerHTML = fmt(l) + '<br>↔ ' + fmt(r) +
        '<div style="color:#8fb3a3;font-size:13px;margin-top:6px">' +
        best.state + (best.nominated ? ' / nominated' : '') + '</div>';
    } catch (e) { row.textContent = 'stats: ' + e.message; }
  }, 700);
}
"""


def caption(page, text):
    page.evaluate(OVERLAY_JS)
    page.evaluate("(t) => { document.querySelector('#demo-caption').textContent = t; }", text)


def wait_connected(page, timeout=45000):
    page.wait_for_function(
        "() => document.querySelector('#wrtc-state') &&"
        " document.querySelector('#wrtc-state').textContent.trim() === 'connected'",
        timeout=timeout,
    )
    page.wait_for_timeout(3000)


def path_text(page):
    return page.text_content("#wrtc-path").strip()


def main():
    shutil.rmtree(RAW_DIR, ignore_errors=True)
    os.makedirs(RAW_DIR, exist_ok=True)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)

    # Start from the live configuration so act 1 is a genuine direct path.
    configure_webrtc(LIVE_STUN, DEVICE_TURN,
                    lambda i: i.get("stun") == LIVE_STUN and i.get("turn"))

    with sync_playwright() as pw:
        browser = pw.chromium.launch(
            headless=True,
            args=[
                "--no-sandbox",
                "--disable-dev-shm-usage",
                "--disable-features=WebRtcHideLocalIpsWithMdns",
            ],
        )
        ctx = browser.new_context(
            viewport=SIZE,
            record_video_dir=RAW_DIR,
            record_video_size=SIZE,
            http_credentials={"username": USER, "password": PASS},
        )
        ctx.add_init_script("try{ localStorage.setItem('mraf-force-relay',''); }catch(_){}")
        page = ctx.new_page()
        page.goto(DEV, wait_until="domcontentloaded")
        page.wait_for_function("typeof webrtc !== 'undefined'", timeout=20000)
        page.wait_for_selector('[data-relay="3"]', timeout=20000)
        caption(page, "qemu_x86 image of the ESP32-class firmware; server advertises STUN and a TURN relay.")
        page.wait_for_timeout(2600)

        # Act 1: live STUN + TURN -> direct path.
        page.click('[data-tab="live"]')
        wait_connected(page)
        direct = path_text(page)
        caption(page, f"Live STUN: selected path {direct} - the relay is allocated but unused.")
        page.mouse.move(640, 380, steps=16)
        page.wait_for_timeout(3200)

        # Act 2: dead STUN, TURN stays up -> relay fallback.
        caption(page, "Point the controller at a dead STUN port; keep the TURN relay.")
        configure_webrtc(DEAD_STUN, DEVICE_TURN,
                        lambda i: i.get("stun") == DEAD_STUN and i.get("turn"))
        page.wait_for_timeout(1400)
        page.click("#webrtc-reconnect")
        wait_connected(page)
        relay = path_text(page)
        caption(page, f"STUN failed, TURN allocation succeeded: selected path {relay}.")
        page.wait_for_timeout(3600)

        # Act 3: force relay on the browser side -> relay <-> relay.
        caption(page, "Force TURN relay: browser policy 'relay' - both sides use the relay.")
        page.check("#wrtc-force-relay")
        wait_connected(page)
        forced = path_text(page)
        caption(page, f"Relay-only ICE pair: {forced} - DTLS/SCTP still carries the dashboard.")
        page.mouse.move(640, 460, steps=16)
        page.wait_for_timeout(3600)

        caption(page, "Same sources build for ESP32-WROOM: 195,272 / 196,608 B DRAM, no PSRAM.")
        page.wait_for_timeout(2600)

        video = page.video.path()
        ctx.close()
        browser.close()

    if not (direct.startswith("direct") and relay.startswith("relay")
            and forced.startswith("relay")):
        print(f"unexpected paths: direct={direct!r} relay={relay!r} forced={forced!r}")
        return 1

    subprocess.run(
        [
            "ffmpeg", "-y", "-loglevel", "error", "-i", video,
            "-c:v", "libx264", "-preset", "medium", "-crf", "22",
            "-pix_fmt", "yuv420p", "-movflags", "+faststart", OUT,
        ],
        check=True,
    )
    print(f"paths: direct={direct!r} relay={relay!r} forced={forced!r}")
    print(f"video: {OUT} ({os.path.getsize(OUT)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
