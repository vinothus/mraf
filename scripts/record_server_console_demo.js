const { chromium } = require('playwright');
const fs = require('fs');

const baseUrl = process.env.MRAF_DEMO_URL || 'http://127.0.0.1:8085/';
const adminUser = process.env.MRAF_DEMO_USER || 'admin';
const adminPass = process.env.MRAF_DEMO_PASS || 'mraf-demo';
const outDir = process.env.MRAF_VIDEO_DIR || '/tmp/mraf-server-console-demo/raw';

const LOGIN_CSS = `
*{box-sizing:border-box}body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;
 background:radial-gradient(1200px 700px at 20% 0%,#dceaff 0%,#eef5ff 45%,#f7fbff 100%);
 font-family:Inter,system-ui,-apple-system,Segoe UI,Roboto,"Noto Sans Tamil",sans-serif;color:#172438}
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
  <div class="brand"><div class="mark">நீ</div><div><h1>நீர்காவலன் Neer Kaavalan</h1><p>நுண் வேளாண்மை Precision Agriculture</p></div></div>
  <div class="role">%ROLE%</div>
  <form>
    <label for="u">Username</label><input id="u" autocomplete="username">
    <label for="p">Password</label><input id="p" type="password" autocomplete="current-password">
    <button id="go" type="button">Sign in</button>
  </form>
  <div class="hint">நுண் வேளாண்மை கட்டுப்பாட்டு மையம் – Smart Agriculture Control Center<br>HTTP Basic authentication &middot; server dashboard<br>%HINT%</div>
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
      cap.textContent = 'Remote Zephyr console for every controller';
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

  async function runCommand(cmd, text) {
    if (text) await caption(text);
    const before = await page.locator('#console-term .term-entry').count();
    await page.locator('#console-input').click();
    await page.locator('#console-input').fill(cmd);
    await pause(280);
    await page.locator('#console-form button[type="submit"]').click();
    await page.waitForFunction(
      (n) => document.querySelectorAll('#console-term .term-entry').length > n,
      before, { timeout: 15000 }
    );
    await pause(1500);
  }

  await login('Admin login', adminUser, adminPass,
    'Credentials from MRAF_DASHBOARD_USER / MRAF_DASHBOARD_PASSWORD');
  const token = Buffer.from(`${adminUser}:${adminPass}`).toString('base64');
  await page.setExtraHTTPHeaders({ Authorization: `Basic ${token}` });
  await page.goto(baseUrl, { waitUntil: 'networkidle' });
  await page.waitForSelector('#admin-view:not([hidden])', { timeout: 30000 });
  await page.waitForSelector('.device-card', { timeout: 30000 });
  await installOverlays();

  await pause(1800);
  await caption('Admin fleet: controllers register themselves over CoAP and wait for approval');
  await page.mouse.move(430, 470, { steps: 20 });
  await pause(2000);

  await caption('Open the Console tab to reach the on-device Zephyr shell of any controller');
  await page.click('[data-admin-tab="console"]');
  await page.waitForSelector('#admin-console:not([hidden])', { timeout: 15000 });
  await page.waitForSelector('.console-device', { timeout: 15000 });
  await page.mouse.move(200, 430, { steps: 20 });
  await pause(2400);

  await caption('Every connected controller is listed here; pick one to attach to its console');
  await page.locator('[data-console-device="SIM-000002"]').click();
  await pause(2400);
  await page.locator('[data-console-device="SIM-000001"]').click();
  await pause(1600);

  await caption('Clear the buffer, then run commands against the selected controller');
  await page.click('#console-clear');
  await pause(1400);

  await runCommand('help', 'Type a command and press Send; the controller executes it on its next poll');
  await runCommand('mraf status', 'Controller status straight from the on-device shell');
  await runCommand('kernel version', 'Zephyr kernel and firmware version reported by the device');
  await runCommand('net iface', 'Network interfaces, including the WireGuard wg0 overlay address');
  await runCommand('mraf relay 5 on', 'Switch a relay from the remote console with the same safety rules');
  await runCommand('mraf wg', 'WireGuard tunnel details pushed by the server');
  await runCommand('uptime', 'Device uptime; output returns on the next CoAP poll');
  await caption('The console round-trips over CoAP (and WireGuard when remote) with an ACK per command');
  await page.mouse.move(720, 560, { steps: 20 });
  await pause(2600);

  console.log(await page.video().path());
  await context.close();
  await browser.close();
})();
