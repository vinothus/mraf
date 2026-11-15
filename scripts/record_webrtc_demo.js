const { chromium } = require('playwright');
const fs = require('fs');

const DEV_URL = process.env.MRAF_DEV_URL || 'http://127.0.0.1:8080/';
const ADMIN_URL = process.env.MRAF_ADMIN_URL || 'http://127.0.0.1:8085/';
const DEV_USER = process.env.MRAF_DEV_USER || 'admin';
const DEV_PASS = process.env.MRAF_DEV_PASS || 'farm1234';
const ADMIN_USER = process.env.MRAF_ADMIN_USER || 'admin';
const ADMIN_PASS = process.env.MRAF_ADMIN_PASS || 'mraf-demo';
const outDir = process.env.MRAF_VIDEO_DIR || '/tmp/mraf-webrtc-demo/raw';

const LOGIN_CSS = `
*{box-sizing:border-box}body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;
 background:radial-gradient(1200px 700px at 20% 0%,#dceaff 0%,#eef5ff 45%,#f7fbff 100%);
 font-family:Inter,system-ui,-apple-system,Segoe UI,Roboto,"Noto Sans Tamil",sans-serif;color:#172438}
.card{width:440px;background:#fff;border:1px solid #d4deeb;border-radius:20px;padding:36px 40px;
 box-shadow:0 30px 80px rgba(23,36,56,.18)}
.brand{display:flex;align-items:center;gap:14px;margin-bottom:8px}
.mark{width:46px;height:46px;border-radius:13px;background:linear-gradient(140deg,#f0a63c,#e8862a);color:#fff;
 display:flex;align-items:center;justify-content:center;font-size:24px;font-weight:800}
h1{font-size:21px;margin:0}p{color:#5d718c;margin:6px 0 0;font-size:13px}
.role{display:inline-block;margin-top:16px;padding:6px 11px;border-radius:999px;background:#e8f1ff;
 color:#1c6fd1;font-size:12px;font-weight:700;letter-spacing:.4px;text-transform:uppercase}
label{display:block;font-size:12px;color:#5d718c;margin:18px 0 7px;font-weight:600}
input{width:100%;padding:13px 14px;border-radius:10px;border:1px solid #cfdbea;background:#fbfdff;color:#172438;font-size:15px;outline:none}
input:focus{border-color:#e8862a;box-shadow:0 0 0 3px #e8862a2e}
button{margin-top:24px;width:100%;padding:14px;border:0;border-radius:11px;background:#e8862a;color:#fff;font-size:15px;font-weight:800;cursor:pointer}
.hint{margin-top:16px;font-size:12px;color:#7286a0;line-height:1.55}
`;

const LOGIN_HTML = `
<!doctype html><html><head><meta charset="utf-8"><style>${LOGIN_CSS}</style></head><body>
<div class="card">
  <div class="brand"><div class="mark">\u0BA8\u0BC0</div><div><h1>\u0BA8\u0BC0\u0BB0\u0BCD\u0B95\u0BBE\u0BB5\u0BB2\u0BA9\u0BCD Neer Kaavalan</h1><p>Precision Agriculture Control Center</p></div></div>
  <div class="role">%ROLE%</div>
  <form>
    <label for="u">Username</label><input id="u" autocomplete="username">
    <label for="p">Password</label><input id="p" type="password" autocomplete="current-password">
    <button id="go" type="button">Sign in</button>
  </form>
  <div class="hint">%HINT%</div>
</div></body></html>
`;

async function login(page, role, user, pass, hint) {
  await page.setContent(LOGIN_HTML.replace('%ROLE%', role).replace('%HINT%', hint));
  await page.waitForTimeout(900);
  await page.locator('#u').click();
  await page.locator('#u').pressSequentially(user, { delay: 60 });
  await page.locator('#p').click();
  await page.locator('#p').pressSequentially(pass, { delay: 60 });
  await page.waitForTimeout(300);
  await page.locator('#go').click();
  await page.waitForTimeout(600);
}

async function installOverlays(page, title) {
  await page.addStyleTag({ content: `
    * { caret-color: transparent !important; }
    #demo-caption {
      position: fixed; left: 22px; bottom: 20px; z-index: 99999; max-width: 82%;
      padding: 11px 16px; border-radius: 9px; color: #14322a; background: rgba(255,255,255,.95);
      border: 1px solid #cfe2d8; border-left: 5px solid #0f9d63;
      box-shadow: 0 12px 34px rgba(23,36,56,.18);
      font: 600 17px/1.35 system-ui, -apple-system, Segoe UI, sans-serif;
    }
    #demo-title {
      position: fixed; left: 22px; top: 16px; z-index: 99999;
      padding: 7px 13px; border-radius: 8px; color: #1c6fd1; background: rgba(232,241,255,.96);
      border: 1px solid #cfe0f5; font: 800 14px/1 system-ui, sans-serif; letter-spacing: .3px;
    }
    #demo-cursor {
      position: fixed; width: 20px; height: 20px; margin: -10px 0 0 -10px;
      border: 3px solid #0f9d63; border-radius: 50%; background: rgba(15,157,99,.16);
      box-shadow: 0 0 0 4px rgba(15,157,99,.18), 0 2px 8px rgba(23,36,56,.24);
      z-index: 100000; pointer-events: none;
    }
  `});
  await page.evaluate((t) => {
    const cap = document.createElement('div');
    cap.id = 'demo-caption'; cap.textContent = ' ';
    document.body.appendChild(cap);
    const ti = document.createElement('div');
    ti.id = 'demo-title'; ti.textContent = t;
    document.body.appendChild(ti);
    const cur = document.createElement('div');
    cur.id = 'demo-cursor'; cur.style.left = '512px'; cur.style.top = '384px';
    document.body.appendChild(cur);
    document.addEventListener('mousemove', (e) => {
      cur.style.left = e.clientX + 'px'; cur.style.top = e.clientY + 'px';
    }, { passive: true });
  }, title);
}

const cap = (page, text) => page.evaluate((t) => {
  const el = document.querySelector('#demo-caption');
  if (el) el.textContent = t;
}, text);

(async () => {
  fs.rmSync(outDir, { recursive: true, force: true });
  fs.mkdirSync(outDir, { recursive: true });

  const browser = await chromium.launch({
    headless: true,
    args: ['--disable-features=WebRtcHideLocalIpsWithMdns'],
  });
  const devCtx = await browser.newContext({
    viewport: { width: 1024, height: 768 },
    recordVideo: { dir: outDir, size: { width: 1024, height: 768 } },
    httpCredentials: { username: DEV_USER, password: DEV_PASS },
  });
  const admCtx = await browser.newContext({
    viewport: { width: 1024, height: 768 },
    recordVideo: { dir: outDir, size: { width: 1024, height: 768 } },
    colorScheme: 'dark',
    httpCredentials: { username: ADMIN_USER, password: ADMIN_PASS },
  });
  const dev = await devCtx.newPage();
  const adm = await admCtx.newPage();
  const pause = (ms) => Promise.all([dev.waitForTimeout(ms), adm.waitForTimeout(ms)]);

  await login(dev, 'Controller dashboard', DEV_USER, DEV_PASS,
    'HTTP Basic \u00b7 on-device dashboard');
  await dev.goto(DEV_URL, { waitUntil: 'domcontentloaded' });
  await dev.waitForSelector('#tab-dash:not([hidden])', { timeout: 20000 });
  await installOverlays(dev, 'Controller \u00b7 on the farm LAN');

  await login(adm, 'Server admin', ADMIN_USER, ADMIN_PASS,
    'HTTP Basic \u00b7 Spring Boot server dashboard');
  await adm.goto(ADMIN_URL, { waitUntil: 'domcontentloaded' });
  await adm.waitForSelector('#admin-view:not([hidden])', { timeout: 30000 });
  await installOverlays(adm, 'Server \u00b7 Spring Boot control plane');

  await cap(dev, 'The controller serves its dashboard over plain HTTP on the LAN.');
  await pause(2200);
  await dev.mouse.move(360, 300, { steps: 18 });
  await pause(1800);

  await cap(dev, 'Open Live P2P: the browser offers, the controller answers.');
  await dev.click('[data-tab="live"]');
  await dev.waitForFunction(
    () => document.querySelector('#wrtc-state') &&
          document.querySelector('#wrtc-state').textContent.trim() === 'connected',
    null, { timeout: 35000 });
  await cap(dev, 'Data channel open \u2014 the dashboard now flows peer-to-peer.');
  await dev.mouse.move(512, 300, { steps: 18 });
  await pause(2600);

  await cap(adm, 'Server: it relayed the handshake and nothing else.');
  await adm.click('[data-admin-tab="live"]');
  await adm.waitForSelector('#live-grid .device-card', { timeout: 15000 });
  await adm.mouse.move(400, 420, { steps: 18 });
  await pause(3200);

  await cap(dev, 'A relay command travels over the encrypted channel, not HTTP.');
  await dev.click('[data-tab="dash"]');
  await pause(1200);
  await dev.click('[data-relay="3"][data-mode="on"]');
  await pause(1400);
  await dev.click('[data-relay="3"][data-mode="off"]');
  await pause(1400);
  await dev.click('[data-tab="live"]');
  await pause(1200);
  await cap(dev, 'Transport stays WebRTC P2P; RTT and byte counters update live.');
  await dev.mouse.move(760, 470, { steps: 18 });
  await pause(2600);

  await cap(adm, 'The server only ever sees link state, RTT and byte counts.');
  await adm.mouse.move(760, 430, { steps: 18 });
  await pause(3000);

  await cap(dev, 'Direct, DTLS-encrypted, and invisible to the server.');
  await pause(2600);

  const devPath = await dev.video().path();
  const admPath = await adm.video().path();
  await devCtx.close();
  await admCtx.close();
  await browser.close();
  fs.copyFileSync(devPath, `${outDir}/../dev.webm`);
  fs.copyFileSync(admPath, `${outDir}/../adm.webm`);
  console.log('device video:', `${outDir}/../dev.webm`);
  console.log('admin video:', `${outDir}/../adm.webm`);
})();
