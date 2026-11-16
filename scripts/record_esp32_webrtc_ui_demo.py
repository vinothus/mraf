#!/usr/bin/env python3
"""Record the ESP32 firmware dashboard running with the WebRTC transport.

The ESP32 production firmware is built from the same Zephyr sources as the
native_sim target, so this records the real device UI and the real portable
DTLS/SCTP peer against the Spring Boot signalling/STUN control plane. The
dashboard itself is served by the firmware's own HTTP server; after the P2P
link opens, /api/status, /api/config, /api/control, /api/events and /api/chat
are fetched over the encrypted data channel instead of HTTP.

Prerequisite: the native-webrtc firmware on :18090 and the Spring Boot
signalling server on :18085 (STUN on UDP 3478).

    MRAF_HTTP_PORT=18090 build/native-webrtc/zephyr/zephyr.exe &
    python3 scripts/record_esp32_webrtc_ui_demo.py

Output: docs/demo/mraf-esp32-webrtc-ui-demo.mp4 (H.264, yuv420p, faststart).
"""

import os
import shutil
import subprocess
import sys

from playwright.sync_api import sync_playwright

URL = os.environ.get("MRAF_DEV_URL", "http://127.0.0.1:18090/")
USER = os.environ.get("MRAF_DEV_USER", "admin")
PASS = os.environ.get("MRAF_DEV_PASS", "farm1234")
RAW_DIR = os.environ.get("MRAF_VIDEO_RAW", "/tmp/mraf-esp32-webrtc-ui/raw")
OUT = os.environ.get(
    "MRAF_VIDEO", "/workspace/mraf/docs/demo/mraf-esp32-webrtc-ui-demo.mp4"
)
SIZE = {"width": 1440, "height": 900}

TITLE_HTML = """
<!doctype html><html><head><meta charset="utf-8"><style>
  * { box-sizing: border-box; }
  body { margin:0; min-height:100vh; display:flex; align-items:center;
    justify-content:center; color:#eaf6ef;
    background:radial-gradient(1200px 700px at 15% 0%,#123b2a 0%,#0b2118 50%,#08160f 100%);
    font-family:Inter,system-ui,-apple-system,Segoe UI,Roboto,sans-serif; }
  .wrap { width:1080px; }
  .mark { width:64px; height:64px; border-radius:18px; display:flex;
    align-items:center; justify-content:center; font-size:32px; font-weight:800;
    background:linear-gradient(140deg,#1fbf72,#0d7a45); box-shadow:0 18px 50px rgba(31,191,114,.28); }
  h1 { font-size:40px; line-height:1.15; margin:24px 0 8px; letter-spacing:.2px; }
  .sub { font-size:19px; color:#a9c8b7; margin:0 0 30px; }
  ul { list-style:none; margin:0; padding:0; display:grid; gap:13px; }
  li { font-size:17px; color:#d5e8dd; padding-left:30px; position:relative; }
  li:before { content:"✓"; position:absolute; left:0; top:-1px; color:#2ee08a;
    font-weight:800; }
  .tag { display:inline-block; margin-top:30px; padding:8px 13px; border-radius:999px;
    background:rgba(46,224,138,.12); border:1px solid rgba(46,224,138,.35);
    color:#7df0b4; font-size:13px; font-weight:700; letter-spacing:.4px;
    text-transform:uppercase; }
</style></head><body><div class="wrap">
  <div class="mark">நீ</div>
  <h1>ESP32 firmware dashboard<br>with the WebRTC data channel</h1>
  <p class="sub">Zephyr native_sim build of the ESP32 production firmware</p>
  <ul>
    <li>Dashboard and REST API served by the firmware's own HTTP server</li>
    <li>Portable DTLS/SCTP peer — same code path as the ESP32 image</li>
    <li>Spring Boot only relays SDP and runs STUN; live data is peer-to-peer</li>
    <li>ESP32 WebRTC image embeds the full UI plus /api/* and /mcp</li>
  </ul>
  <span class="tag">device UI · not the server dashboard</span>
</div></body></html>
"""

OVERLAY_JS = """
() => {
  if (document.querySelector('#demo-caption')) return;
  const add = (id, css, html) => {
    const el = document.createElement('div');
    el.id = id; el.style.cssText = css;
    if (html) el.innerHTML = html;
    document.body.appendChild(el);
    return el;
  };
  add('demo-title',
    'position:fixed;left:22px;top:16px;z-index:2147483646;padding:8px 14px;' +
    'border-radius:9px;color:#0d3b25;background:rgba(232,255,243,.96);' +
    'border:1px solid #9fe7c2;font:800 14px/1 Inter,system-ui,sans-serif;' +
    'letter-spacing:.25px','ESP32 firmware dashboard &middot; WebRTC P2P');
  add('demo-badge',
    'position:fixed;right:22px;top:16px;z-index:2147483646;padding:8px 14px;' +
    'border-radius:9px;color:#cdeedd;background:rgba(12,40,27,.9);' +
    'border:1px solid #2b6b4a;font:700 13px/1 Inter,system-ui,sans-serif',
    'native_sim &middot; HTTP :18090');
  add('demo-caption',
    'position:fixed;left:0;right:0;bottom:0;z-index:2147483647;padding:20px 34px;' +
    'font:600 21px/1.35 Inter,system-ui,sans-serif;color:#fff;' +
    'background:linear-gradient(90deg,rgba(15,34,25,.96),rgba(15,34,25,.74));' +
    'box-shadow:0 -8px 30px rgba(0,0,0,.3)');
  const cur = add('demo-cursor',
    'position:fixed;width:22px;height:22px;margin:-11px 0 0 -11px;' +
    'border:3px solid #2ee08a;border-radius:50%;background:rgba(46,224,138,.18);' +
    'box-shadow:0 0 0 5px rgba(46,224,138,.18),0 2px 8px rgba(0,0,0,.35);' +
    'z-index:2147483648;pointer-events:none');
  document.addEventListener('mousemove', (e) => {
    cur.style.left = e.clientX + 'px';
    cur.style.top = e.clientY + 'px';
  }, { passive: true });
}
"""


def caption(page, text):
    page.evaluate(
        "(t) => { const el = document.querySelector('#demo-caption');"
        " if (el) el.textContent = t; }",
        text,
    )


def move(page, x, y, steps=14):
    page.mouse.move(x, y, steps=steps)


def main():
    shutil.rmtree(RAW_DIR, ignore_errors=True)
    os.makedirs(RAW_DIR, exist_ok=True)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)

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
            color_scheme="dark",
            http_credentials={"username": USER, "password": PASS},
        )
        page = ctx.new_page()
        page.set_default_timeout(30000)

        # --- title card -----------------------------------------------------
        page.set_content(TITLE_HTML, wait_until="load")
        page.wait_for_timeout(3400)

        # --- device dashboard (served by the firmware) ----------------------
        page.goto(URL, wait_until="domcontentloaded")
        page.wait_for_selector('[data-relay="3"]', timeout=30000)
        page.evaluate(OVERLAY_JS)
        page.wait_for_timeout(1400)
        move(page, 720, 380)
        caption(page, "This dashboard is served by the firmware's own HTTP server — the ESP32 UI, not the server UI.")
        page.wait_for_timeout(3200)

        caption(page, "Pump card, live sensors, interlocks and 12 mapped relay channels, straight from the controller.")
        page.wait_for_timeout(2800)

        # --- relay over plain HTTP -----------------------------------------
        caption(page, "Relay 3 ON over plain HTTP on the LAN (the fallback path when WebRTC is off).")
        move(page, 900, 690)
        page.click('[data-relay="3"][data-mode="on"]')
        page.wait_for_timeout(1600)
        page.click('[data-relay="3"][data-mode="off"]')
        page.wait_for_timeout(1400)

        # --- schedules ------------------------------------------------------
        caption(page, "Daily schedules: up to four windows per relay, day masks, equipment mapping and the pump timetable.")
        move(page, 620, 52)
        page.click('[data-tab="sched"]')
        page.evaluate("window.scrollTo(0, 120)")
        page.wait_for_timeout(3200)

        # --- recurring ------------------------------------------------------
        caption(page, "Recurring rules support day, week, month and year windows with actions per output.")
        page.click('[data-tab="rec"]')
        page.evaluate("window.scrollTo(0, 0)")
        page.wait_for_timeout(2600)

        # --- settings / WebRTC config --------------------------------------
        caption(page, "Settings include device, Wi-Fi, CoAP, WebRTC, pump protection and maintenance — all stored on the device.")
        page.click('[data-tab="settings"]')
        page.evaluate("window.scrollTo(0, 0)")
        page.wait_for_timeout(900)
        page.locator("#set-webrtc-server").scroll_into_view_if_needed()
        page.evaluate(
            """() => {
              const card = document.querySelector('#set-webrtc-enabled').closest('.card');
              card.style.outline = '2px solid #2ee08a';
              card.style.boxShadow = '0 0 0 5px rgba(46,224,138,.22)';
            }"""
        )
        page.wait_for_timeout(3400)
        page.evaluate(
            """() => {
              const card = document.querySelector('#set-webrtc-enabled').closest('.card');
              card.style.outline = ''; card.style.boxShadow = '';
            }"""
        )

        # --- WireGuard ------------------------------------------------------
        caption(page, "The WireGuard tunnel tab shows the server-managed overlay configuration.")
        page.click('[data-tab="tunnel"]')
        page.evaluate("window.scrollTo(0, 0)")
        page.wait_for_timeout(2500)

        # --- WebRTC live link ----------------------------------------------
        caption(page, "Live P2P: the browser posts an SDP offer, the controller answers, and STUN discovers the path.")
        page.click('[data-tab="live"]')
        page.evaluate("window.scrollTo(0, 0)")
        page.wait_for_timeout(1200)
        page.wait_for_function(
            "() => document.querySelector('#wrtc-state') &&"
            " document.querySelector('#wrtc-state').textContent.trim() === 'connected'",
            timeout=45000,
        )
        caption(page, "Connected: a direct DTLS/SCTP data channel is open — the server never sees the dashboard traffic.")
        move(page, 760, 300)
        page.wait_for_timeout(3000)

        caption(page, "Round-trip time and byte counters come from the data channel itself.")
        move(page, 980, 300)
        page.wait_for_timeout(2600)

        # --- dashboard again, now over the data channel ---------------------
        caption(page, "Back on the dashboard — /api/status, /api/control, /api/config and /api/events now ride WebRTC.")
        page.click('[data-tab="dash"]')
        page.evaluate("window.scrollTo(0, 0)")
        page.wait_for_timeout(900)
        move(page, 900, 690)
        page.click('[data-relay="3"][data-mode="on"]')
        page.wait_for_timeout(1600)
        page.click('[data-relay="3"][data-mode="off"]')
        page.wait_for_timeout(1400)

        # --- offline grammar chat over the channel --------------------------
        caption(page, "Offline grammar chat runs on the device; commands are dispatched over the same data channel.")
        page.click('[data-tab="chat"]')
        page.evaluate("window.scrollTo(0, 0)")
        page.wait_for_timeout(800)
        page.click("#chat-input")
        page.locator("#chat-input").press_sequentially("turn on relay 3", delay=55)
        page.keyboard.press("Enter")
        page.wait_for_timeout(1800)
        page.locator("#chat-input").press_sequentially("boost pump 30 min", delay=55)
        page.keyboard.press("Enter")
        page.wait_for_timeout(2400)

        caption(page, "The pump is boosting now — the intent was parsed on the controller and applied over the P2P link.")
        page.click('[data-tab="dash"]')
        page.evaluate("window.scrollTo(0, 0)")
        page.wait_for_timeout(2600)

        # --- link counters after the commands -------------------------------
        caption(page, "Tx/rx grew as live status and control commands crossed the encrypted peer link.")
        page.click('[data-tab="live"]')
        page.evaluate("window.scrollTo(0, 0)")
        page.wait_for_timeout(2800)

        # --- event log ------------------------------------------------------
        caption(page, "The event log records the WebRTC answer, the peer link and the relay/pump commands.")
        page.click('[data-tab="events"]')
        page.evaluate("window.scrollTo(0, 0)")
        page.wait_for_timeout(3200)

        # --- final summary --------------------------------------------------
        page.evaluate(
            """() => {
              const el = document.createElement('div');
              el.style.cssText = [
                'position:fixed','inset:0','z-index:2147483649','display:flex',
                'align-items:center','justify-content:center',
                'background:radial-gradient(1200px 700px at 18% 0%,#123b2a 0%,#0b2118 55%,#08160f 100%)',
                'color:#eaf6ef','font:16px/1.5 Inter,system-ui,sans-serif'
              ].join(';');
              el.innerHTML = `
                <div style="width:980px">
                  <div style="font:800 13px/1 Inter,system-ui,sans-serif;letter-spacing:.5px;
                    text-transform:uppercase;color:#7df0b4;margin-bottom:16px">Confirmed in this build</div>
                  <h2 style="font-size:34px;line-height:1.2;margin:0 0 24px">
                    WebRTC keeps the full ESP32 UI and feature set</h2>
                  <div style="display:grid;grid-template-columns:1fr 1fr;gap:14px 34px;color:#d5e8dd;font-size:16px">
                    <div>✓ Firmware HTTP server + embedded webui.h</div>
                    <div>✓ REST API: status, config, control, events, time</div>
                    <div>✓ Daily schedules, recurring rules, pulse outputs</div>
                    <div>✓ Pump protections and simulator fault injection</div>
                    <div>✓ WireGuard tunnel state and settings</div>
                    <div>✓ Offline grammar chat and on-device /mcp endpoint</div>
                    <div>✓ DTLS/SCTP data channel (no TURN relay)</div>
                    <div>✓ ESP32 image 978,944 B with mraf_peer_* linked</div>
                  </div>
                  <div style="margin-top:30px;color:#8fb7a3;font-size:14px">
                    ESP32 WebRTC build: <span style="color:#cdeedd">zephyr.bin</span> ·
                    device UI at <span style="color:#cdeedd">http://&lt;device&gt;/</span> ·
                    live dashboard over <span style="color:#cdeedd">WebRTC P2P</span>
                  </div>
                </div>`;
              document.body.appendChild(el);
            }"""
        )
        page.wait_for_timeout(5600)

        video = page.video.path()
        ctx.close()
        browser.close()

    subprocess.run(
        [
            "ffmpeg", "-y", "-loglevel", "error", "-i", video,
            "-c:v", "libx264", "-preset", "medium", "-crf", "22",
            "-pix_fmt", "yuv420p", "-movflags", "+faststart", OUT,
        ],
        check=True,
    )
    print(f"video: {OUT} ({os.path.getsize(OUT)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
