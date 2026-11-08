const { chromium } = require('playwright');
const fs = require('fs');

(async () => {
  const outDir = process.env.MRAF_VIDEO_DIR || '/tmp/mraf-server-demo/raw';
  const baseUrl = process.env.MRAF_DEMO_URL || 'http://127.0.0.1:8080/';
  const username = process.env.MRAF_DEMO_USER || 'admin';
  const password = process.env.MRAF_DEMO_PASS || 'mraf-demo';
  fs.rmSync(outDir, { recursive: true, force: true });
  fs.mkdirSync(outDir, { recursive: true });

  const browser = await chromium.launch({ headless: true });
  const context = await browser.newContext({
    viewport: { width: 1440, height: 900 },
    recordVideo: { dir: outDir, size: { width: 1440, height: 900 } },
    httpCredentials: { username, password },
  });
  const page = await context.newPage();

  const caption = async (text) => page.evaluate((t) => {
    const el = document.querySelector('#demo-caption');
    if (el) el.textContent = t;
  }, text);

  const pause = (ms) => page.waitForTimeout(ms);

  await page.goto(baseUrl, { waitUntil: 'networkidle' });
  await page.waitForSelector('#link-pill.on', { timeout: 30000 });
  await page.waitForSelector('.device-card', { timeout: 20000 });

  const pendingConfirm = await page.$('.device-card.pending [data-confirm]');
  if (pendingConfirm) {
    await pendingConfirm.click();
    await page.waitForTimeout(1500);
  }
  const openBtn = await page.$('.device-card [data-open]');
  if (openBtn) {
    await openBtn.click();
  } else {
    await page.click('.device-card');
  }
  await page.waitForSelector('#device-view:not([hidden])', { timeout: 10000 });

  await page.addStyleTag({ content: `
    * { caret-color: transparent !important; }
    #demo-caption {
      position: fixed; left: 28px; bottom: 24px; z-index: 99999;
      max-width: 76%; padding: 13px 20px; border-radius: 10px;
      color: #eafff4; background: rgba(4, 24, 17, 0.88);
      border-left: 5px solid #34d399; box-shadow: 0 10px 30px rgba(0,0,0,.35);
      font: 600 21px/1.35 system-ui, -apple-system, Segoe UI, sans-serif;
      letter-spacing: .1px; transition: opacity .2s;
    }
    #demo-cursor {
      position: fixed; width: 22px; height: 22px; margin: -11px 0 0 -11px;
      border: 3px solid #ffffff; border-radius: 50%; background: rgba(52,211,153,.28);
      box-shadow: 0 0 0 4px rgba(52,211,153,.22), 0 2px 8px rgba(0,0,0,.35);
      z-index: 100000; pointer-events: none;
    }
  `});
  await page.evaluate(() => {
    const cap = document.createElement('div');
    cap.id = 'demo-caption';
    cap.textContent = 'Spring Boot dashboard polling the ESP32 controller over CoAP';
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

  await pause(2500);
  await caption('Live telemetry: battery, mains, pump, floats and all 12 relays');
  await page.mouse.move(520, 430, { steps: 20 });
  await pause(2500);

  await caption('Requesting a fresh snapshot over the queued CoAP poll');
  await page.click('#request-data');
  await pause(3500);
  await caption('The controller acknowledges on the next poll; the snapshot appears automatically');
  await pause(2200);

  await caption('Remote relay control uses the same safety rules as the ESP32 dashboard');
  await page.click('[data-relay="4"][data-mode="on"]');
  await pause(3200);
  await caption('Relay 4 is ON, confirmed by an ACK from the controller');
  await pause(2200);

  await caption('Pump control is queued, then confirmed the same way');
  await page.click('[data-pump="on"]');
  await pause(3200);
  await page.click('[data-pump="auto"]');
  await pause(2500);

  await caption('Schedules tab: relay number to GPIO mapping, day masks and four daily windows');
  await page.click('[data-tab="sched"]');
  await pause(2500);
  await page.locator('#schedules [data-channel="1"] .name').fill('Orchard Light');
  await caption('Editing a schedule locally in the browser');
  await pause(1600);

  await page.locator('#save-config').scrollIntoViewIfNeeded();
  await page.click('#save-config');
  await pause(1200);
  await caption('Saving sends the complete validated configuration over CoAP');
  await page.mouse.move(1180, 160, { steps: 20 });
  await pause(3200);
  await page.locator('#pump-schedule').scrollIntoViewIfNeeded();
  await pause(3000);
  await caption('Pump protection is mirrored too: dry-run, max run, min off and outage recovery');
  await pause(2600);

  await caption('Activity log shows every queued request, poll and acknowledgement');
  await page.click('[data-tab="activity"]');
  await pause(3200);
  await page.mouse.move(360, 520, { steps: 18 });
  await pause(2300);

  await page.click('[data-tab="dash"]');
  await pause(1500);
  await caption('One remote dashboard for live data, control, schedules and relay mapping');
  await page.mouse.move(760, 655, { steps: 22 });
  await pause(3500);

  await context.close();
  const video = page.video();
  const videoPath = await video.path();
  console.log(`raw video: ${videoPath}`);
  await browser.close();
})();
