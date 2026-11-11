const { chromium } = require('playwright');
const fs = require('fs');

const baseUrl = process.env.MRAF_DEMO_URL || 'http://127.0.0.1:8080/';
const adminUser = process.env.MRAF_DEMO_USER || 'admin';
const adminPass = process.env.MRAF_DEMO_PASS || 'mraf-demo';
const deviceUser = process.env.MRAF_DEMO_DEVICE_USER || 'farm';
const devicePass = process.env.MRAF_DEMO_DEVICE_PASS || 'farm1234';
const outDir = process.env.MRAF_VIDEO_DIR || '/tmp/mraf-server-demo/raw';

const LOGIN_CSS = `
*{box-sizing:border-box}body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;
background:radial-gradient(1200px 700px at 20% 0%,#dceaff 0%,#eef5ff 45%,#f7fbff 100%);
font-family:Inter,system-ui,-apple-system,Segoe UI,Roboto,sans-serif;color:#172438}
.card{width:470px;background:#ffffff;border:1px solid #d4deeb;border-radius:20px;padding:38px 42px;
box-shadow:0 30px 80px rgba(23,36,56,.18)}
.brand{display:flex;align-items:center;gap:14px;margin-bottom:8px}
.mark{width:46px;height:46px;border-radius:13px;background:linear-gradient(140deg,#f0a63c,#e8862a);color:#fff;
display:flex;align-items:center;justify-content:center;font-size:24px;font-weight:800}
h1{font-size:22px;margin:0;letter-spacing:.2px}p{color:#5d718c;margin:6px 0 0;font-size:13px}
.role{display:inline-block;margin-top:18px;padding:6px 11px;border-radius:999px;background:#e8f1ff;
color:#1c6fd1;font-size:12px;font-weight:700;letter-spacing:.4px;text-transform:uppercase}
label{display:block;font-size:12px;color:#5d718c;margin:20px 0 7px;font-weight:600}
input{width:100%;padding:13px 14px;border-radius:10px;border:1px solid #cfdbea;background:#fbfdff;
color:#172438;font-size:15px;outline:none}input:focus{border-color:#e8862a;box-shadow:0 0 0 3px #e8862a2e}
button{margin-top:26px;width:100%;padding:14px;border:0;border-radius:11px;background:#e8862a;color:#fff;
font-size:15px;font-weight:800;cursor:pointer}button:focus{outline:3px solid #e8862a88;outline-offset:3px}
.hint{margin-top:18px;font-size:12px;color:#7286a0;line-height:1.55}
`;

const LOGIN_HTML = `
<!doctype html><html><head><meta charset="utf-8"><style>${LOGIN_CSS}</style></head><body>
<div class="card">
  <div class="brand"><div class="mark">M</div><div><h1>MRAF Remote</h1><p>Multi-device ESP32 fleet &middot; CoAP &middot; Spring Boot + H2</p></div></div>
  <div class="role">%ROLE%</div>
  <form>
    <label for="u">Username</label><input id="u" autocomplete="username">
    <label for="p">Password</label><input id="p" type="password" autocomplete="current-password">
    <button id="go" type="button">Sign in</button>
  </form>
  <div class="hint">HTTP Basic authentication &middot; server dashboard<br>%HINT%</div>
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
    httpCredentials: { username: adminUser, password: adminPass },
  });
  const page = await context.newPage();

  const pause = (ms) => page.waitForTimeout(ms);
  const caption = (text) => page.evaluate((t) => {
    const el = document.querySelector('#demo-caption');
    if (el) el.textContent = t;
  }, text);

  async function login(role, user, pass, hint) {
    await page.setContent(LOGIN_HTML.replace('%ROLE%', role).replace('%HINT%', hint));
    await pause(900);
    await page.locator('#u').click();
    await page.locator('#u').pressSequentially(user, { delay: 70 });
    await page.locator('#p').click();
    await page.locator('#p').pressSequentially(pass, { delay: 70 });
    await pause(400);
    await page.locator('#go').evaluate((el) => {
      el.style.outline = '3px solid #f0a63c';
      el.style.outlineOffset = '4px';
    });
    await pause(450);
    await page.locator('#go').click();
    await pause(700);
  }

  async function auth(user, pass) {
    const token = Buffer.from(`${user}:${pass}`).toString('base64');
    await page.setExtraHTTPHeaders({ Authorization: `Basic ${token}` });
  }

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
      cap.textContent = 'Spring Boot remote dashboard over CoAP';
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

  await login('Admin login', adminUser, adminPass,
    'Credentials from MRAF_DASHBOARD_USER / MRAF_DASHBOARD_PASSWORD');
  await auth(adminUser, adminPass);
  await page.goto(baseUrl, { waitUntil: 'networkidle' });
  await page.waitForSelector('#admin-view:not([hidden])', { timeout: 30000 });
  await page.waitForSelector('.device-card', { timeout: 30000 });
  await installOverlays();

  await pause(2200);
  await caption('Admin fleet: controllers register themselves and wait for approval');
  await page.mouse.move(430, 470, { steps: 20 });
  await pause(2400);

  await caption('Theme selection: one click switches the whole dashboard to the bright theme');
  await page.locator('#theme-toggle').scrollIntoViewIfNeeded();
  await page.mouse.move(1150, 40, { steps: 22 });
  await pause(900);
  await page.click('#theme-toggle');
  await waitLight();
  await page.mouse.move(1180, 90, { steps: 12 });
  await pause(2600);

  await caption('The bright theme is remembered in this browser');
  await page.reload({ waitUntil: 'networkidle' });
  await page.waitForSelector('#admin-view:not([hidden])', { timeout: 20000 });
  await waitLight();
  await installOverlays();
  await pause(2400);

  const pending = page.locator('.device-card.pending [data-confirm]').first();
  if (await pending.count()) {
    await caption('Confirm the pending ESP32; its device login is created at the same time');
    await pending.scrollIntoViewIfNeeded();
    await page.mouse.move(520, 560, { steps: 20 });
    await pause(1200);
    await pending.click();
    await page.waitForSelector('.device-card:not(.pending)', { timeout: 20000 });
    await pause(2200);
  }

  await caption('Open the confirmed controller dashboard');
  await page.locator('.device-card [data-open]').first().click();
  await page.waitForSelector('#device-view:not([hidden])', { timeout: 15000 });
  await page.waitForFunction(
    () => document.querySelector('#stat-battery').textContent.trim() !== '--',
    { timeout: 20000 }
  );
  await page.mouse.move(720, 470, { steps: 20 });
  await pause(2000);

  await caption('Live telemetry: battery, mains, pump, floats and all 12 outputs');
  await page.mouse.move(520, 430, { steps: 20 });
  await pause(2600);

  await caption('Requesting a fresh snapshot over the queued CoAP poll');
  await page.click('#request-data');
  await pause(3400);
  await caption('The controller acknowledges on the next poll; the snapshot updates automatically');
  await pause(2200);

  await caption('Remote relay control uses the same safety rules as the ESP32 dashboard');
  await page.click('[data-relay="4"][data-mode="on"]');
  await pause(3200);
  await caption('Relay 4 is ON, confirmed by an ACK from the controller');
  await pause(2000);

  await caption('Pump control: on, boost and back to auto, all queued over CoAP');
  await page.click('[data-pump="on"]');
  await pause(2800);
  await page.click('[data-pump="boost"]');
  await pause(2400);
  await page.click('[data-pump="auto"]');
  await pause(2200);

  await caption('Schedules: relay to GPIO mapping, zones, equipment, day masks and four windows');
  await page.click('[data-tab="sched"]');
  await pause(2200);
  const ch1 = page.locator('#schedules [data-channel="1"]');
  await ch1.locator('.name').fill('Orchard Light');
  await ch1.locator('.zone').fill('Orchard');
  await ch1.locator('.equipment').selectOption('light');
  await ch1.locator('.day').first().check();
  await caption('Editing a schedule locally in the bright theme');
  await pause(1800);

  await page.locator('#pump-schedule').scrollIntoViewIfNeeded();
  await caption('Pump protection is mirrored too: dry-run, max run, min off and outage recovery');
  await pause(2200);
  await page.locator('#save-config').scrollIntoViewIfNeeded();
  await page.mouse.move(1180, 150, { steps: 20 });
  await pause(600);
  await page.click('#save-config');
  await caption('Save & send validates and pushes the complete configuration over CoAP');
  await pause(2800);

  await caption('Recurring schedules: daily, weekly, monthly or yearly windows');
  await page.click('[data-tab="rec"]');
  await pause(2200);
  await page.click('#add-rec');
  await pause(1200);
  const rec = page.locator('#recurring [data-rec]').last();
  await rec.locator('.rname').fill('Weekly valve run');
  await rec.locator('.rrepeat').selectOption('weekly');
  await rec.locator('.rfrom').fill('05:30');
  await rec.locator('.rto').fill('06:15');
  await rec.locator('.rch').first().check();
  await caption('A new weekly recurring rule, added from the server');
  await pause(2000);
  await page.locator('#save-config-rec').scrollIntoViewIfNeeded();
  await page.click('#save-config-rec');
  await pause(2600);

  await caption('Activity: every queued request, poll and acknowledgement');
  await page.click('[data-tab="activity"]');
  await pause(2600);
  await page.mouse.move(430, 520, { steps: 18 });
  await pause(2400);

  await page.click('[data-tab="dash"]');
  await pause(1400);
  await caption('One remote dashboard for live data, control, schedules and relay mapping');
  await page.mouse.move(760, 655, { steps: 22 });
  await pause(2400);

  await caption('Back to the fleet for the device-user view');
  await page.click('#back-btn');
  await page.waitForSelector('#admin-view:not([hidden])', { timeout: 15000 });
  await pause(1800);

  await auth(deviceUser, devicePass);
  await login('Device user login', deviceUser, devicePass,
    'User created when the admin confirmed SIM-000001');
  await page.goto(baseUrl, { waitUntil: 'networkidle' });
  await page.waitForSelector('#device-view:not([hidden])', { timeout: 20000 });
  await waitLight();
  await installOverlays();
  await page.waitForFunction(
    () => document.querySelector('#stat-battery').textContent.trim() !== '--',
    { timeout: 20000 }
  );
  await caption('The confirmed device user gets the same bright dashboard and live controls');
  await page.mouse.move(600, 480, { steps: 20 });
  await pause(2200);

  await page.click('[data-relay="1"][data-mode="on"]');
  await pause(2600);
  await page.click('[data-pump="on"]');
  await pause(2800);
  await caption('Relay and pump commands confirmed by the controller over CoAP');
  await page.mouse.move(760, 655, { steps: 22 });
  await pause(3200);

  console.log(await page.video().path());
  await context.close();
  await browser.close();
})();
