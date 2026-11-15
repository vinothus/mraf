#!/usr/bin/env python3
"""Record a short demo of the native_sim firmware dashboard over WebRTC.

The firmware transport carries a data channel only (no audio/video tracks), so
this records the dashboard itself: the browser connects peer-to-peer to the
native_sim controller, then status and relay commands flow over the channel.

Prerequisite: the native-webrtc firmware on :18090 and the signalling server.

    MRAF_HTTP_PORT=18090 build/native-webrtc/zephyr/zephyr.exe &
    python3 scripts/record_native_webrtc_demo.py

Output: docs/demo/mraf-native-webrtc-demo.mp4 (H.264, yuv420p, faststart).
"""

import os
import shutil
import subprocess
import sys

from playwright.sync_api import sync_playwright

URL = os.environ.get("MRAF_DEV_URL", "http://127.0.0.1:18090/")
USER = os.environ.get("MRAF_DEV_USER", "admin")
PASS = os.environ.get("MRAF_DEV_PASS", "farm1234")
RAW_DIR = os.environ.get("MRAF_VIDEO_RAW", "/tmp/mraf-native-webrtc/raw")
OUT = os.environ.get(
    "MRAF_VIDEO", "/workspace/mraf/docs/demo/mraf-native-webrtc-demo.mp4"
)
SIZE = {"width": 1280, "height": 800}

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
}
"""


def caption(page, text):
    page.evaluate(OVERLAY_JS)
    page.evaluate(
        "(t) => { document.querySelector('#demo-caption').textContent = t; }", text
    )


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
            http_credentials={"username": USER, "password": PASS},
        )
        page = ctx.new_page()
        page.goto(URL, wait_until="domcontentloaded")
        page.wait_for_function("typeof webrtc !== 'undefined'", timeout=20000)

        caption(page, "native_sim firmware serving its dashboard over plain HTTP.")
        page.wait_for_selector('[data-relay="3"]', timeout=20000)
        page.wait_for_timeout(2200)

        caption(page, "Live P2P: the browser offers, the controller answers (no TURN).")
        page.click('[data-tab="live"]')
        page.wait_for_function(
            "() => document.querySelector('#wrtc-state') &&"
            " document.querySelector('#wrtc-state').textContent.trim() === 'connected'",
            timeout=40000,
        )
        page.wait_for_timeout(1800)
        caption(page, "Data channel open: the dashboard now flows peer-to-peer.")
        page.mouse.move(640, 360, steps=18)
        page.wait_for_timeout(2600)

        caption(page, "A relay command travels over DTLS/SCTP, not HTTP.")
        page.click('[data-tab="dash"]')
        page.wait_for_timeout(900)
        page.click('[data-relay="3"][data-mode="on"]')
        page.wait_for_timeout(1300)
        page.click('[data-relay="3"][data-mode="off"]')
        page.wait_for_timeout(1300)

        caption(page, "Transport stays WebRTC P2P; tx/rx and RTT update live.")
        page.click('[data-tab="live"]')
        page.mouse.move(640, 470, steps=18)
        page.wait_for_timeout(2600)

        caption(page, "Status, config, events and chat all ride the data channel.")
        page.click('[data-tab="chat"]')
        page.wait_for_timeout(1500)
        page.click('[data-tab="live"]')
        page.wait_for_timeout(2600)

        caption(page, "Chrome <-> ESP32-class peer: plain Zephyr, mbedTLS, no PSRAM.")
        page.wait_for_timeout(2400)

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
