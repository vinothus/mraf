#!/usr/bin/env python3
"""Record configurable dashboard heading / subheading on both UIs.

The video shows the ESP32 firmware dashboard (served by the device over HTTP)
and the Spring Boot server UI side by side. It starts from the built-in
defaults, saves custom values in each UI's settings, then reloads / signs in
again to prove the values are persisted and used on the next load.

Prerequisite:
    MRAF_HTTP_PORT=18090 build/native/zephyr/zephyr.exe &
    MRAF_HTTP_PORT=18085 MRAF_DB_URL='jdbc:h2:mem:branding;DB_CLOSE_DELAY=-1' \
        java -jar server/target/mraf-server-1.0.0.jar &

Output: docs/demo/customheaderandsubheader.mp4
"""

import base64
import json
import os
import shutil
import subprocess
import sys
import urllib.request

from playwright.sync_api import sync_playwright

DEV_URL = os.environ.get("MRAF_DEV_URL", "http://127.0.0.1:18090/")
SRV_URL = os.environ.get("MRAF_SRV_URL", "http://127.0.0.1:18085/")
DEV_USER = os.environ.get("MRAF_DEV_USER", "admin")
DEV_PASS = os.environ.get("MRAF_DEV_PASS", "farm1234")
SRV_USER = os.environ.get("MRAF_SRV_USER", "admin")
SRV_PASS = os.environ.get("MRAF_SRV_PASS", "mraf-demo")
RAW_DIR = os.environ.get("MRAF_VIDEO_RAW", "/tmp/mraf-custom-header/raw")
OUT = os.environ.get(
    "MRAF_VIDEO", "/workspace/mraf/docs/demo/customheaderandsubheader.mp4"
)
SIZE = {"width": 1280, "height": 800}

DEV_HEADING = "நீலம் பண்ணை"
DEV_SUBHEADING = "Blue Farm Controller · Zone 3"
SRV_HEADING = "நீலம் கட்டுப்பாட்டு மையம்"
SRV_SUBHEADING = "Blue Farm Remote Console · Chennai"


def basic(user, passwd):
    token = base64.b64encode(f"{user}:{passwd}".encode()).decode()
    return {"Authorization": "Basic " + token}


def http_json(url, payload=None, user=DEV_USER, passwd=DEV_PASS, method=None):
    data = None if payload is None else json.dumps(payload).encode()
    req = urllib.request.Request(url, data=data, method=method or ("POST" if data else "GET"))
    req.add_header("Content-Type", "application/json")
    for k, v in basic(user, passwd).items():
        req.add_header(k, v)
    with urllib.request.urlopen(req, timeout=15) as res:
        body = res.read().decode()
    return json.loads(body) if body else {}


def reset_defaults():
    http_json(f"{DEV_URL}api/config", {"device": {"heading": "நீர்காவலன்", "subheading": ""}})
    http_json(
        f"{SRV_URL}api/branding",
        {"heading": "", "subheading": ""},
        user=SRV_USER,
        passwd=SRV_PASS,
        method="PUT",
    )


OVERLAY_CSS = """
* { caret-color: transparent !important; }
#demo-tag {
  position: fixed; left: 22px; top: 16px; z-index: 2147483646;
  padding: 8px 14px; border-radius: 9px; color: #0d3b25;
  background: rgba(232,255,243,.96); border: 1px solid #9fe7c2;
  font: 800 14px/1 Inter,system-ui,sans-serif; letter-spacing: .25px;
}
#demo-caption {
  position: fixed; left: 0; right: 0; bottom: 0; z-index: 2147483647;
  padding: 18px 30px; color: #fff;
  background: linear-gradient(90deg,rgba(15,34,25,.97),rgba(15,34,25,.78));
  box-shadow: 0 -8px 30px rgba(0,0,0,.35);
  font: 600 20px/1.35 Inter,system-ui,sans-serif;
}
#demo-cursor {
  position: fixed; width: 22px; height: 22px; margin: -11px 0 0 -11px;
  border: 3px solid #2ee08a; border-radius: 50%; background: rgba(46,224,138,.18);
  box-shadow: 0 0 0 5px rgba(46,224,138,.18),0 2px 8px rgba(0,0,0,.35);
  z-index: 2147483648; pointer-events: none;
}
"""


def install_overlay(page, tag):
    page.add_style_tag(content=OVERLAY_CSS)
    page.evaluate(
        """(tag) => {
          const add = (id, text) => {
            const el = document.createElement('div');
            el.id = id;
            el.textContent = text;
            document.body.appendChild(el);
            return el;
          };
          add('demo-tag', tag);
          add('demo-caption', ' ');
          const cur = add('demo-cursor', '');
          document.addEventListener('mousemove', (e) => {
            cur.style.left = e.clientX + 'px';
            cur.style.top = e.clientY + 'px';
          }, { passive: true });
        }""",
        tag,
    )


def caption(page, text):
    page.evaluate(
        """(t) => { const el = document.querySelector('#demo-caption');
                   if (el) el.textContent = t; }""",
        text,
    )


def move(page, x, y):
    page.mouse.move(x, y, steps=14)


def type_text(page, selector, text):
    page.click(selector)
    page.fill(selector, "")
    page.locator(selector).press_sequentially(text, delay=28)


def main():
    shutil.rmtree(RAW_DIR, ignore_errors=True)
    os.makedirs(RAW_DIR, exist_ok=True)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)

    reset_defaults()

    with sync_playwright() as pw:
        browser = pw.chromium.launch(
            headless=True,
            args=[
                "--no-sandbox",
                "--disable-dev-shm-usage",
                "--disable-features=WebRtcHideLocalIpsWithMdns",
            ],
        )
        dev_ctx = browser.new_context(
            viewport=SIZE,
            record_video_dir=f"{RAW_DIR}/device",
            record_video_size=SIZE,
            color_scheme="dark",
            http_credentials={"username": DEV_USER, "password": DEV_PASS},
        )
        srv_ctx = browser.new_context(
            viewport=SIZE,
            record_video_dir=f"{RAW_DIR}/server",
            record_video_size=SIZE,
            color_scheme="dark",
            http_credentials={"username": SRV_USER, "password": SRV_PASS},
        )
        dev = dev_ctx.new_page()
        srv = srv_ctx.new_page()
        dev.set_default_timeout(30000)
        srv.set_default_timeout(30000)

        # --- device UI: the default header -------------------------------
        dev.goto(DEV_URL, wait_until="domcontentloaded")
        dev.wait_for_selector("#brand-head", timeout=30000)
        dev.wait_for_timeout(1800)
        install_overlay(dev, "ESP32 firmware dashboard · device UI")
        move(dev, 700, 320)
        caption(dev, "ESP32 dashboard — the heading and subheading currently come from the built-in defaults.")
        dev.wait_for_timeout(3200)

        # --- server UI: the default header --------------------------------
        srv.goto(SRV_URL, wait_until="domcontentloaded")
        srv.wait_for_selector("#admin-view:not([hidden])", timeout=30000)
        srv.wait_for_timeout(1600)
        install_overlay(srv, "Remote server · Spring Boot UI")
        move(srv, 700, 320)
        caption(srv, "Remote server — same default heading and subheading in the dashboard header.")
        srv.wait_for_timeout(3000)

        # --- device settings ---------------------------------------------
        caption(dev, "Open Settings on the controller. Heading and subheading are now editable fields.")
        move(dev, 560, 52)
        dev.click('[data-tab="settings"]')
        dev.wait_for_timeout(700)
        dev.locator("#set-heading").scroll_into_view_if_needed()
        dev.wait_for_timeout(1200)
        caption(dev, "Type a custom dashboard heading and subheading, then save the settings.")
        type_text(dev, "#set-heading", DEV_HEADING)
        dev.wait_for_timeout(500)
        type_text(dev, "#set-subheading", DEV_SUBHEADING)
        dev.wait_for_timeout(700)
        move(dev, 1180, 52)
        dev.click("#save-settings")
        dev.wait_for_timeout(2200)
        caption(dev, "Saved on the controller — the values are written to its settings file.")
        dev.wait_for_timeout(2600)

        # --- device dashboard after save ---------------------------------
        move(dev, 320, 52)
        dev.click('[data-tab="dash"]')
        dev.wait_for_timeout(900)
        move(dev, 420, 250)
        caption(dev, "The device dashboard header now shows the custom heading and subheading.")
        dev.wait_for_timeout(3200)

        # --- device reload: persistence ----------------------------------
        dev.reload(wait_until="domcontentloaded")
        dev.wait_for_selector("#brand-head", timeout=30000)
        dev.wait_for_timeout(1600)
        install_overlay(dev, "ESP32 firmware dashboard · device UI")
        move(dev, 420, 250)
        caption(dev, "Reloaded: the next load reads the saved settings again.")
        dev.wait_for_timeout(3200)

        # --- server settings ---------------------------------------------
        caption(srv, "On the server, open the admin Settings tab.")
        move(srv, 760, 100)
        srv.click('[data-admin-tab="settings"]')
        srv.wait_for_timeout(900)
        move(srv, 520, 300)
        caption(srv, "The same heading and subheading fields are stored in the server database.")
        srv.wait_for_timeout(2400)
        type_text(srv, "#brand-heading-input", SRV_HEADING)
        srv.wait_for_timeout(400)
        type_text(srv, "#brand-subheading-input", SRV_SUBHEADING)
        srv.wait_for_timeout(600)
        move(srv, 1000, 190)
        srv.click("#save-branding")
        srv.wait_for_timeout(1800)
        caption(srv, "Saved. The preview and the dashboard header update immediately.")
        srv.wait_for_timeout(2800)

        # --- server sign-in screen ---------------------------------------
        move(srv, 1140, 52)
        srv.click("#signout")
        srv.wait_for_timeout(900)
        caption(srv, "The sign-in screen renders the same custom heading and subheading from the server settings.")
        move(srv, 640, 300)
        srv.wait_for_timeout(3600)

        # --- sign back in -------------------------------------------------
        caption(srv, "Signing back in confirms the setting is live, not a one-off page change.")
        type_text(srv, "#login-user", SRV_USER)
        type_text(srv, "#login-pass", SRV_PASS)
        srv.wait_for_timeout(300)
        srv.click('#login-form button[type="submit"]')
        srv.wait_for_timeout(2200)
        move(srv, 700, 320)
        caption(srv, "Signed in again — the dashboard header still uses the saved custom branding.")
        srv.wait_for_timeout(3000)

        # --- device: show the saved values in settings -------------------
        move(dev, 560, 52)
        dev.click('[data-tab="settings"]')
        dev.wait_for_timeout(700)
        dev.locator("#set-heading").scroll_into_view_if_needed()
        dev.wait_for_timeout(600)
        caption(dev, "Both UIs now take the heading and subheading from their own settings.")
        dev.wait_for_timeout(3400)

        # --- final frame ---------------------------------------------------
        dev.evaluate(
            """() => {
              const el = document.createElement('div');
              el.style.cssText = [
                'position:fixed','inset:0','z-index:2147483649','display:flex',
                'align-items:center','justify-content:center',
                'background:radial-gradient(1200px 700px at 18% 0%,#123b2a 0%,#0b2118 55%,#08160f 100%)',
                'color:#eaf6ef','font:16px/1.5 Inter,system-ui,sans-serif'
              ].join(';');
              el.innerHTML = `
                <div style="width:900px">
                  <div style="font:800 13px/1 Inter,sans-serif;letter-spacing:.5px;
                    text-transform:uppercase;color:#7df0b4;margin-bottom:14px">Confirmed</div>
                  <h2 style="font-size:34px;line-height:1.2;margin:0 0 20px">
                    Heading and subheading now come from settings</h2>
                  <div style="display:grid;gap:11px;color:#d5e8dd;font-size:17px">
                    <div>✓ ESP32 dashboard reads them from the device configuration</div>
                    <div>✓ Remote server reads them from its persisted branding settings</div>
                    <div>✓ Defaults keep the original header text when unset</div>
                    <div>✓ Custom values survive the next load and the sign-in screen</div>
                  </div>
                </div>`;
              document.body.appendChild(el);
            }"""
        )
        dev.wait_for_timeout(3200)
        srv.wait_for_timeout(3200)

        dev_video = dev.video.path()
        srv_video = srv.video.path()
        dev_ctx.close()
        srv_ctx.close()
        browser.close()

    # --- side-by-side composition ----------------------------------------
    subprocess.run(
        [
            "ffmpeg", "-y", "-loglevel", "error",
            "-i", dev_video, "-i", srv_video,
            "-filter_complex",
            "[0:v]scale=960:600,setsar=1[d];"
            "[1:v]scale=960:600,setsar=1[s];"
            "[d][s]hstack=inputs=2[v]",
            "-map", "[v]", "-shortest",
            "-c:v", "libx264", "-preset", "medium", "-crf", "22",
            "-pix_fmt", "yuv420p", "-movflags", "+faststart", OUT,
        ],
        check=True,
    )
    print(f"video: {OUT} ({os.path.getsize(OUT)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
