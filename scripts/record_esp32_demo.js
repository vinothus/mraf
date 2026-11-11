const { chromium } = require('playwright');
const fs = require('fs');

const baseUrl = process.env.MRAF_DEMO_URL || 'http://127.0.0.1:18082/';
const username = process.env.MRAF_DEMO_USER || 'admin';
const password = process.env.MRAF_DEMO_PASS || 'farm1234';
const outDir = process.env.MRAF_VIDEO_DIR || '/tmp/mraf-esp32-demo/raw';

const LOGIN_CSS = `
*{box-sizing:border-box}body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;
background:radial-gradient(1100px 650px at 18% 0%,#d9f4e6 0%,#eef8f2 50%,#f7fcf9 100%);
font-family:Inter,system-ui,-apple-system,Segoe UI,Roboto,sans-serif;color:#15251d}
.card{width:470px;background:#ffffff;border:1px solid #d3e1d9;border-radius:20px;padding:38px 42px;
box-shadow:0 30px 80px rgba(21,37,29,.16)}
.brand{display:flex;align-items:center;gap:14px;margin-bottom:8px}
.mark{width:46px;height:46px;border-radius:13px;background:linear-gradient(140deg,#19a45d,#0d7040);color:#fff;
display:flex;align-items:center;justify-content:center;font-size:24px;font-weight:800}
h1{font-size:22px;margin:0;letter-spacing:.2px}p{color:#5f766b;margin:6px 0 0;font-size:13px}
.role{display:inline-block;margin-top:18px;padding:6px 11px;border-radius:999px;background:#e5f5ec;
color:#0d7040;font-size:12px;font-weight:700;letter-spacing:.4px;text-transform:uppercase}
label{display:block;font-size:12px;color:#5f766b;margin:20px 0 7px;font-weight:600}
input{width:100%;padding:13px 14px;border-radius:10px;border:1px solid #cfdfd6;background:#fbfdfc;
color:#15251d;font-size:15px;outline:none}input:focus{border-color:#19a45d;box-shadow:0 0 0 3px #19a45d2e}
button{margin-top:26px;width:100%;padding:14px;border:0;border-radius:11px;background:#19a45d;color:#fff;
font-size:15px;font-weight:800;cursor:pointer}button:focus{outline:3px solid #19a45d88;outline-offset:3px}
.hint{margin-top:18px;font-size:12px;color:#779084;line-height:1.55}
`;

const LOGIN_HTML = `
<!doctype html><html><head><meta charset="utf-8"><style>${LOGIN_CSS}</style></head><body>
<div class="card">
  <div class="brand"><div class="mark">M</div><div><h1>MRAF Controller</h1><p>ESP32 web server &middot; local dashboard</p></div></div>
  <div class="role">ESP32 local login</div>
  <form>
    <label for="u">Username</label><input id="u" autocomplete="username">
    <label for="p">Password</label><input id="p" type="password" autocomplete="current-password">
    <button id="go" type="button">Sign in</button>
  </form>
  <div class="hint">HTTP Basic authentication &middot; served by the device<br>admin / farm1234 &middot; native_sim build of the ESP32 firmware</div>
</div></body></html>
`;

(async () => {
  fs.rmSync(outDir, { recursive: true, force: true });
  fs.mkdirSync(outDir, { recursive: true });

  const browser = await chromium.launch({ headless: true });
  const context = await browser.newContext({
    viewport: { width: 1440, height: 900 },
    recordVideo: { dir: outDir, size: { width: 1440, height: 900 } },
    colorScheme: 'dark',
    httpCredentials: { username, password },
  });
  const page = await context.newPage();

  const pause = (ms) => page.waitForTimeout(ms);
  const caption = (text) => page.evaluate((t) => {
    const el = document.querySelector('#demo-caption');
    if (el) el.textContent = t;
  }, text);

  async function installOverlays() {
    await page.addStyleTag({ content: `
      * { caret-color: transparent !important; }
      #demo-caption {
        position: fixed; left: 28px; bottom: 24px; z-index: 99999;
        max-width: 78%; padding: 13px 20px; border-radius: 10px;
        color: #14322a; background: rgba(255,255,255,.95);
        border: 1px solid #cfe2d8; border-left: 5px solid #0f9d63;
        box-shadow: 0 12px 34px rgba(23,36,56,.18);
        font: 600 21px/1.35 system-ui, -apple-system, Segoe UI, sans-serif;
        letter-spacing: .1px; transition: opacity .2s;
      }
      #demo-cursor {
        position: fixed; width: 22px; height: 22px; margin: -11px 0 0 -11px;
        border: 3px solid #0f9d63; border-radius: 50%; background: rgba(15,157,99,.16);
        box-shadow: 0 0 0 4px rgba(15,157,99,.18), 0 2px 8px rgba(23,36,56,.24);
        z-index: 100000; pointer-events: none;
      }
    `});
    await page.evaluate(() => {
      const cap = document.createElement('div');
      cap.id = 'demo-caption';
      cap.textContent = 'ESP32 device dashboard, served from the controller itself';
      document.body.appendChild(cap);
      const cur = document.createElement('div');
      cur.id = 'demo-cursor';
      cur.style.left = '720px';
      cur.style.top = '450px';
      document.body.appendChild(cur);
      document.addEventListener('mousemove', (e) => {
        cur.style.left = e.clientX + 'px';
        cur.style.top = e.clientY + 'px';
      }, { passive: true });
    });
  }

  async function waitLight() {
    await page.waitForFunction(() => document.documentElement.dataset.theme === 'light', { timeout: 10000 });
  }

  await page.setContent(LOGIN_HTML);
  await pause(1000);
  await page.locator('#u').click();
  await page.locator('#u').pressSequentially(username, { delay: 75 });
  await page.locator('#p').click();
  await page.locator('#p').pressSequentially(password, { delay: 75 });
  await pause(400);
  await page.locator('#go').evaluate((el) => {
    el.style.outline = '3px solid #19a45d';
    el.style.outlineOffset = '4px';
  });
  await pause(450);
  await page.locator('#go').click();
  await pause(700);

  await page.goto(baseUrl, { waitUntil: 'networkidle' });
  await page.waitForSelector('#relays .eq-card', { timeout: 20000 });
  await installOverlays();
  await pause(2200);

  await caption('Theme selection: switch the device dashboard to the bright theme');
  await page.locator('#theme-toggle').scrollIntoViewIfNeeded();
  await page.mouse.move(1180, 40, { steps: 20 });
  await pause(800);
  await page.click('#theme-toggle');
  await waitLight();
  await page.mouse.move(1200, 92, { steps: 12 });
  await pause(2400);

  await caption('The bright theme is remembered by the browser');
  await page.reload({ waitUntil: 'networkidle' });
  await page.waitForSelector('#relays .eq-card', { timeout: 20000 });
  await waitLight();
  await installOverlays();
  await pause(2200);

  await caption('Live status: mains, battery, Wi-Fi, remote link, time and active equipment');
  await page.mouse.move(520, 330, { steps: 20 });
  await pause(2400);

  await caption('Pump control with the same protections as the wall panel');
  await page.locator('#pump-name').scrollIntoViewIfNeeded();
  await page.mouse.move(680, 420, { steps: 18 });
  await pause(900);
  await page.locator('[data-pump="on"]').click();
  await pause(2600);
  await page.locator('[data-pump="boost"]').click();
  await pause(2400);
  await page.locator('[data-pump="auto"]').click();
  await pause(2200);

  await caption('Relays: auto, on and off for every mapped load');
  await page.locator('#relays').scrollIntoViewIfNeeded();
  await pause(800);
  await page.locator('[data-relay="1"][data-mode="on"]').click();
  await pause(2400);
  await page.locator('[data-relay="2"][data-mode="on"]').click();
  await pause(2400);
  await page.mouse.move(900, 640, { steps: 20 });
  await pause(1800);

  if (await page.locator('#sim-panel:not([hidden])').count()) {
    await caption('Built-in simulator: inject a full tank float to test the interlock');
    await page.locator('#sim-panel').scrollIntoViewIfNeeded();
    await pause(1000);
    await page.locator('#sim-tank').check();
    await pause(2600);
    await page.locator('#sim-tank').uncheck();
    await pause(2000);
  }

  await caption('Daily schedules: name, equipment, zone, day mask and up to four windows');
  await page.click('nav button[data-tab="sched"]');
  await pause(2200);
  const row = page.locator('#schedules .sched-row').first();
  await row.locator('.name').fill('Orchard Light');
  await row.locator('.equipment').selectOption('light');
  await row.locator('.zone').fill('Orchard');
  await row.locator('.day').first().check();
  await pause(2000);
  await page.locator('#save-config').scrollIntoViewIfNeeded();
  await page.click('#save-config');
  await caption('Save all writes the validated configuration to the controller');
  await pause(2600);

  await caption('Recurring schedules: weekly, monthly and yearly rules');
  await page.click('nav button[data-tab="rec"]');
  await pause(2000);
  await page.click('#add-rec');
  await pause(1200);
  let rec = page.locator('#recurring [data-rec]').last();
  await rec.locator('.rec-name').fill('Weekly valve run');
  await rec.locator('.rec-repeat').selectOption('weekly');
  await pause(900);
  rec = page.locator('#recurring [data-rec]').last();
  await rec.locator('.rec-from').fill('05:30');
  await rec.locator('.rec-to').fill('06:15');
  await rec.locator('.rec-ch').first().check();
  await pause(1800);
  await page.locator('#save-rec').scrollIntoViewIfNeeded();
  await page.click('#save-rec');
  await pause(2600);

  await caption('Settings: device profile, Wi-Fi, remote CoAP server and pump protection');
  await page.click('nav button[data-tab="settings"]');
  await pause(2200);
  await page.mouse.move(720, 420, { steps: 18 });
  await pause(1800);
  await page.locator('#set-minoff').scrollIntoViewIfNeeded();
  await page.mouse.move(720, 470, { steps: 18 });
  await pause(1800);
  await page.locator('#set-time').scrollIntoViewIfNeeded();
  await page.mouse.move(720, 500, { steps: 18 });
  await pause(1800);

  await caption('Events: every schedule, protection trip and control change');
  await page.click('nav button[data-tab="events"]');
  await pause(2400);
  await page.mouse.move(720, 500, { steps: 18 });
  await pause(2200);

  await page.click('nav button[data-tab="dash"]');
  await pause(1400);
  await caption('Light theme across dashboard, schedules, settings and events');
  await page.locator('#relays').scrollIntoViewIfNeeded();
  await page.mouse.move(760, 620, { steps: 22 });
  await pause(3200);

  console.log(await page.video().path());
  await context.close();
  await browser.close();
})();
