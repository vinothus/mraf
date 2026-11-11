const $ = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];
const DAY_BITS = [1, 2, 4, 8, 16, 32, 64];
const DAYS = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"];
const MODES = ["auto", "on", "off"];
const PROFILES = ["agri", "home", "godown", "industry"];
const EQUIPMENT = ["valve", "light", "motor", "fan", "pump", "socket", "heater", "door", "other"];
const REPEATS = ["daily", "weekly", "monthly", "yearly"];
const ACTIONS = ["on", "off", "auto"];
const THEME_KEY = "mraf-theme";

let me = null;
let overview = null;
let deviceId = null;
let snap = null;
let draft = null;
let draftDirty = false;
let editorsRendered = false;
let configSig = "";
let activeTab = "dash";
let toastTimer = null;

function esc(v) {
  return String(v ?? "").replace(/[&<>"']/g, c => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c]));
}

function applyTheme(theme, persist = false) {
  const light = theme === "light";
  document.documentElement.dataset.theme = light ? "light" : "dark";
  document.documentElement.style.colorScheme = light ? "light" : "dark";
  const btn = $("#theme-toggle");
  if (btn) {
    btn.textContent = light ? "Dark theme" : "Light theme";
    btn.setAttribute("aria-label", light ? "Switch to dark theme" : "Switch to light theme");
  }
  if (persist) {
    try { localStorage.setItem(THEME_KEY, light ? "light" : "dark"); } catch (_) {}
  }
}

function initTheme() {
  let saved = null;
  try { saved = localStorage.getItem(THEME_KEY); } catch (_) {}
  const current = document.documentElement.dataset.theme;
  applyTheme(saved === "light" || saved === "dark" ? saved : (current === "light" ? "light" : "dark"));
}

function showToast(message, error = false) {
  const el = $("#toast");
  el.textContent = message;
  el.className = "toast" + (error ? " err" : "");
  el.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.hidden = true; }, 4200);
}

async function api(path, options = {}) {
  const res = await fetch(path, {
    credentials: "same-origin",
    headers: {"Content-Type": "application/json", ...(options.headers || {})},
    ...options
  });
  let body = null;
  try { body = await res.json(); } catch (_) {}
  if (!res.ok) {
    throw new Error((body && (body.message || body.error)) || `${res.status} ${res.statusText}`);
  }
  return body;
}

function icon(kind) {
  const svg = {
    light: `<svg viewBox="0 0 48 48" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"><path d="M18 30a10 10 0 1 1 12 0c-1.4 1-2 2.4-2 4H20c0-1.6-.6-3-2-4Z"/><path d="M20 38h8M21 42h6"/></svg>`,
    fan: `<svg viewBox="0 0 48 48" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"><circle cx="24" cy="24" r="3.5"/><path d="M24 20c-2-8 2-12 6-10 4 2 1 8-6 10Z"/><path d="M28 26c7-2 12 1 11 5-1 4-8 3-11-5Z"/><path d="M20 26c-7-2-12 1-11 5 1 4 8 3 11-5Z"/></svg>`,
    motor: `<svg viewBox="0 0 48 48" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"><circle cx="24" cy="24" r="11"/><path d="M24 16v8l6 4"/><path d="M24 5v4M24 39v4M5 24h4M39 24h4"/></svg>`,
    pump: `<svg viewBox="0 0 48 48" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"><circle cx="24" cy="24" r="10"/><path d="M24 14c5 4 5 16 0 20-5-4-5-16 0-20Z"/><path d="M14 24c4-5 16-5 20 0-4 5-16 5-20 0Z"/></svg>`,
    valve: `<svg viewBox="0 0 48 48" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"><circle cx="24" cy="22" r="10"/><path d="M24 12v20M14 22h20M24 32v10M18 42h12"/></svg>`,
    socket: `<svg viewBox="0 0 48 48" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"><rect x="13" y="13" width="22" height="22" rx="6"/><path d="M20 20v6M28 20v6M24 28v4"/></svg>`,
    heater: `<svg viewBox="0 0 48 48" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"><path d="M12 34c4-6-4-10 0-16M24 34c4-6-4-10 0-16M36 34c4-6-4-10 0-16"/><path d="M8 40h32"/></svg>`,
    door: `<svg viewBox="0 0 48 48" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"><path d="M14 40V8h18v32"/><path d="M32 24h2"/><path d="M14 40h20"/></svg>`,
    other: `<svg viewBox="0 0 48 48" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"><rect x="10" y="12" width="28" height="24" rx="4"/><path d="M17 24h14M24 18v12"/></svg>`
  }[kind] || "";
  const anim = {light: "glow", heater: "glow", fan: "spin", motor: "spin", pump: "spin", valve: "spin", socket: "pulse", door: "swing", other: "pulse"}[kind] || "pulse";
  return `<div class="eq-icon ${anim}">${svg}</div>`;
}

function dayChecks(days, cls) {
  return DAYS.map((d, i) =>
    `<label><input type="checkbox" class="${cls}" value="${i}" ${days & DAY_BITS[i] ? "checked" : ""}> ${d}</label>`
  ).join("");
}

function windowRow(w = {on: "06:00", off: "07:00"}) {
  return `<div class="win"><input type="time" class="won" value="${esc(w.on)}"><input type="time" class="woff" value="${esc(w.off)}"><button class="btn small delwin" type="button">Remove</button></div>`;
}

function readDays(row, cls) {
  return $$(`.${cls}`, row).reduce((n, el) => n | (el.checked ? DAY_BITS[Number(el.value)] : 0), 0);
}

function readWindows(host) {
  return $$(".win", host).map(w => ({on: $(".won", w).value, off: $(".woff", w).value})).filter(w => w.on && w.off);
}

function optionList(list, value) {
  return list.map(v => `<option value="${v}" ${v === value ? "selected" : ""}>${v}</option>`).join("");
}

// ---------------------------------------------------------------- bootstrap
async function init() {
  try {
    me = await api("/api/me");
  } catch (e) {
    $("#login-view").hidden = false;
    return;
  }
  $("#user-name").textContent = me.username;
  $("#role-pill").textContent = me.role === "admin" ? "Admin" : "Device user";
  $("#role-pill").className = "pill " + (me.role === "admin" ? "info" : "on");

  if (me.role === "admin") {
    $("#admin-view").hidden = false;
    await loadOverview();
  } else if (me.deviceId) {
    await openDevice(me.deviceId);
  } else {
    $("#login-view").hidden = false;
  }
  setInterval(refresh, 2000);
}

async function refresh() {
  try {
    if (deviceId) await loadDevice(false);
    else if (me && me.role === "admin") await loadOverview();
  } catch (e) {
    const pill = $("#link-pill");
    if (pill) { pill.textContent = "Server unreachable"; pill.className = "pill offline"; }
  }
}

// ------------------------------------------------------------------- fleet
async function loadOverview() {
  overview = await api("/api/devices");
  renderOverview();
}

function renderOverview() {
  const o = overview;
  $("#adm-total").textContent = o.deviceCount;
  $("#adm-confirmed").textContent = o.confirmedCount;
  $("#adm-online").textContent = o.onlineCount;
  $("#adm-pending").textContent = (o.devices || []).filter(d => !d.confirmed && !d.rejected).length;
  $("#link-pill").textContent = o.onlineCount ? `${o.onlineCount} online` : "Waiting for controllers";
  $("#link-pill").className = "pill " + (o.onlineCount ? "on" : "offline");

  const grid = $("#device-grid");
  const devices = o.devices || [];
  $("#fleet-empty").hidden = devices.length > 0;
  grid.innerHTML = devices.map(d => {
    const pending = !d.confirmed && !d.rejected;
    return `<article class="card device-card ${pending ? "pending" : ""}" data-device="${esc(d.id)}">
      <div class="row">
        <span class="pill ${d.online ? "on" : "offline"}">${d.online ? "online" : "offline"}</span>
        <span class="id">${esc(d.id)}</span>
      </div>
      <h3>${esc(d.name || d.id)}</h3>
      <div class="hint">${esc(d.firmware || "firmware unknown")} · poll ${esc(d.lastPoll || "never")}</div>
      <div class="meta">
        <span class="pill info">${esc(d.profile)}</span>
        ${d.confirmed ? `<span class="pill on">confirmed</span>` : `<span class="pill warn">${d.rejected ? "rejected" : "pending"}</span>`}
        <span class="pill">${d.queued || 0} queued</span>
      </div>
      <div class="actions">
        ${pending ? `<button class="btn small primary" data-confirm="${esc(d.id)}" type="button">Confirm</button>
                     <button class="btn small danger" data-reject="${esc(d.id)}" type="button">Reject</button>` : ""}
        <button class="btn small" data-open="${esc(d.id)}" type="button">Open dashboard</button>
      </div>
    </article>`;
  }).join("");
}

function backToFleet() {
  deviceId = null; snap = null; draft = null; editorsRendered = false;
  $("#device-view").hidden = true;
  $("#admin-view").hidden = false;
  loadOverview();
}

async function openDevice(id) {
  deviceId = id;
  draft = null; draftDirty = false; editorsRendered = false; configSig = "";
  $("#admin-view").hidden = true;
  $("#login-view").hidden = true;
  $("#device-view").hidden = false;
  $("#back-btn").hidden = me.role !== "admin";
  switchTab("dash");
  await loadDevice(true);
}

// ------------------------------------------------------------------ device
async function loadDevice(initial) {
  snap = await api(`/api/devices/${encodeURIComponent(deviceId)}`);
  renderHeader();
  renderDashboard();

  const sig = JSON.stringify(snap.config);
  if (initial || (!draftDirty && sig !== configSig)) {
    configSig = sig;
    draft = structuredClone(snap.config);
    editorsRendered = false;
  }
  if (!draftDirty && (activeTab === "sched" || activeTab === "rec") && !editorsRendered) {
    renderEditors();
  }
  if (activeTab === "activity") renderActivity();
}

function renderHeader() {
  const s = snap;
  $("#dev-id").textContent = s.id;
  $("#dev-name").textContent = s.name || s.id;
  $("#dev-link").textContent = s.online ? "Online" : "Waiting";
  $("#dev-link").className = "pill " + (s.online ? "on" : "offline");
  $("#dev-confirmed").textContent = s.confirmed ? "confirmed" : (s.rejected ? "rejected" : "pending");
  $("#dev-confirmed").className = "pill " + (s.confirmed ? "on" : (s.rejected ? "bad" : "warn"));
  $("#dev-profile").textContent = s.profile;
  $("#dev-profile").className = "pill info";
  $("#link-pill").textContent = s.online ? "Controller online" : "Waiting for controller";
  $("#link-pill").className = "pill " + (s.online ? "on" : "offline");

  const admin = me.role === "admin";
  $("#dev-confirm").hidden = !admin || s.confirmed;
  $("#dev-reject").hidden = !admin || s.confirmed;
  $("#dev-delete").hidden = !admin;
}

function renderDashboard() {
  const s = snap;
  const d = s.lastData;
  $("#stat-battery").textContent = d && d.battery_v != null ? Number(d.battery_v).toFixed(2) + " V" : "--";
  $("#stat-power").textContent = d ? (d.power ? "Present" : "OUTAGE") : "--";
  $("#stat-power-sub").textContent = d ? (d.power ? "battery charging" : "on LiFePO4 backup") : "controller-only backup";

  const relayStates = d && Array.isArray(d.relays) ? d.relays : [];
  $("#stat-active").textContent = d ? String(relayStates.filter(v => Number(v) === 1).length) : "--";

  const q = s.request || {};
  $("#stat-queue").textContent = String(q.inflight ? 1 : (q.queued || 0));
  $("#stat-queue-sub").textContent = q.inflight
    ? `${q.inflightType} ${q.inflightId} · attempt ${q.inflightAttempts}`
    : (q.queued ? `${q.queued} queued` : "no pending request");

  const pump = (d && d.pump) || {};
  const cfgPump = (s.config && s.config.pump) || {};
  $("#pump-name").textContent = cfgPump.name || "Pump";
  $("#pump-mode").textContent = pump.mode || cfgPump.mode || "--";
  $("#pump-current").textContent = pump.current_a != null ? Number(pump.current_a).toFixed(1) + " A" : "--";
  $("#pump-sched").textContent = pump.on ? "RUNNING" : (pump.state || "OFF");
  $("#pump-fault").textContent = pump.fault || "none";
  $("#pump-state").textContent = pump.on ? "RUNNING" : (pump.state || "OFF");
  $("#pump-state").className = "badge " + (pump.on ? "on" : "off");
  const tank = d ? d.tank_full : null;
  const sump = d ? d.sump_dry : null;
  $("#float-tank").textContent = tank == null ? "Tank: --" : (tank ? "Tank FULL" : "Tank OK");
  $("#float-tank").className = "pill " + (tank ? "warn" : "");
  $("#float-sump").textContent = sump == null ? "Sump: --" : (sump ? "Sump DRY" : "Sump OK");
  $("#float-sump").className = "pill " + (sump ? "warn" : "");

  $("#link-last").textContent = s.lastPoll || "--";
  $("#link-ack").textContent = q.lastAck ? `${q.lastAck.id} → ${q.lastAck.result}` : "--";
  $("#link-flight").textContent = q.inflight ? `${q.inflightType} ${q.inflightId}` : "none";
  $("#link-polls").textContent = String(s.pollCount ?? 0);
  $("#link-interval").textContent = `${s.pollIntervalSec ?? 60} s`;

  const channels = (s.config && s.config.channels) || [];
  const relayMap = s.relayMap || [];
  $("#relays").innerHTML = channels.map((ch, i) => {
    const map = relayMap.find(r => r.id === ch.id) || {};
    const on = Number(relayStates[i] || 0) === 1;
    const windows = (ch.windows || []).map(w => `${w.on}–${w.off}`).join(", ") || "no window";
    const pulse = ch.pulse_mode ? `<button class="btn small accent" data-pulse="${ch.id}" data-ms="${ch.pulse_on_ms || 600}" type="button">Pulse ${ch.pulse_on_ms || 600}ms</button>` : "";
    return `<article class="card relay ${on ? "on" : ""}" data-relay-card="${ch.id}">
      <div class="relay-top"><div class="relay-id">Relay ${ch.id} <small>· GPIO ${map.gpio ?? "?"}</small></div>
      <div class="rstate"><span class="dot"></span>${on ? "ON" : "OFF"}</div></div>
      ${icon(ch.equipment)}
      <div class="relay-name">${esc(ch.name)}</div>
      <div class="relay-meta">${esc(ch.zone || "no zone")} · ${esc(ch.equipment)} · ${esc(ch.mode)}${ch.enabled ? "" : " · disabled"}<br>${esc(windows)}</div>
      <div class="btnrow">
        <button class="btn small" data-relay="${ch.id}" data-mode="auto" type="button">Auto</button>
        <button class="btn small" data-relay="${ch.id}" data-mode="on" type="button">On</button>
        <button class="btn small" data-relay="${ch.id}" data-mode="off" type="button">Off</button>
        ${pulse}
      </div>
    </article>`;
  }).join("");
}

// ---------------------------------------------------------------- editors
function renderEditors() {
  renderSchedules();
  renderRecurring();
  $("#profile-select").value = draft.profile || "agri";
  $("#interval-input").value = snap.pollIntervalSec ?? 60;
  editorsRendered = true;
  draftDirty = false;
}

function renderSchedules() {
  $("#schedules").innerHTML = (draft.channels || []).map(ch => `
    <div class="card sched-row" data-channel="${ch.id}">
      <div class="sched-head">
        <div class="map">Relay <b>${ch.id}</b></div>
        <input class="name" type="text" maxlength="23" value="${esc(ch.name)}" aria-label="Relay ${ch.id} name">
        <input class="zone" type="text" maxlength="23" value="${esc(ch.zone || "")}" placeholder="Zone / room / bay">
        <select class="equipment" aria-label="Equipment">${optionList(EQUIPMENT, ch.equipment)}</select>
        <select class="mode" aria-label="Mode">${optionList(MODES, ch.mode)}</select>
        <label class="check"><input class="enabled" type="checkbox" ${ch.enabled ? "checked" : ""}> Enabled</label>
      </div>
      <div class="days">${dayChecks(ch.days || 0, "day")}</div>
      <div class="windows">${(ch.windows || []).map(windowRow).join("")}</div>
      <div class="btnrow">
        <button class="btn small addwin" type="button">+ Add window</button>
        <label class="check"><input class="pulse" type="checkbox" ${ch.pulse_mode ? "checked" : ""}> Momentary pulse</label>
        <label class="check">on <input class="pon" type="number" min="20" max="10000" step="10" value="${ch.pulse_on_ms ?? 600}"> ms</label>
        <label class="check">off <input class="poff" type="number" min="20" max="10000" step="10" value="${ch.pulse_off_ms ?? 600}"> ms</label>
      </div>
    </div>`).join("");

  const p = draft.pump || {};
  $("#pump-schedule").innerHTML = `
    <div class="sched-head">
      <div class="map">Relay <b>13</b><br>pump contactor</div>
      <input class="pname" type="text" maxlength="23" value="${esc(p.name || "Pump")}" aria-label="Pump name">
      <select class="pmode">${optionList(MODES, p.mode)}</select>
      <label class="check"><input class="penabled" type="checkbox" ${p.enabled ? "checked" : ""}> Enabled</label>
    </div>
    <div class="days">${dayChecks(p.days || 0, "pday")}</div>
    <div class="windows pump-windows">${(p.windows || []).map(windowRow).join("")}</div>
    <div class="btnrow"><button class="btn small paddwin" type="button">+ Add pump window</button></div>
    <div class="pump-form">
      <div class="field"><label>Min off (sec)</label><input class="pminoff" type="number" min="30" max="3600" value="${p.min_off_sec ?? 300}"></div>
      <div class="field"><label>Max run (min)</label><input class="pmaxrun" type="number" min="1" max="720" value="${p.max_run_min ?? 120}"></div>
      <div class="field"><label>Recovery delay (sec)</label><input class="precovery" type="number" min="0" max="600" value="${p.recovery_delay_sec ?? 30}"></div>
      <div class="field"><label>Dry-run current (A)</label><input class="pdry" type="number" step="0.1" min="0.5" max="20" value="${p.dry_run_current_a ?? 1.5}"></div>
      <div class="field"><label>Dry-run grace (sec)</label><input class="pgrace" type="number" min="5" max="120" value="${p.dry_run_grace_sec ?? 20}"></div>
    </div>
    <label class="check"><input class="presume" type="checkbox" ${p.resume_after_outage ? "checked" : ""}> Resume pump after outage</label>`;
}

function recRow(rs) {
  const mask = rs.channel_mask || 0;
  const channels = Array.from({length: 12}, (_, i) => i + 1)
    .map(n => `<label><input type="checkbox" class="rch" value="${n}" ${mask & (1 << (n - 1)) ? "checked" : ""}> R${n}</label>`)
    .join("");
  return `<div class="card rec-row" data-rec>
    <div class="rec-head">
      <input class="rname" type="text" maxlength="23" value="${esc(rs.name)}" placeholder="Schedule name">
      <select class="rrepeat">${optionList(REPEATS, rs.repeat)}</select>
      <select class="raction">${optionList(ACTIONS, rs.action)}</select>
      <label class="check"><input class="renabled" type="checkbox" ${rs.enabled ? "checked" : ""}> Enabled</label>
      <button class="btn small danger" data-remrec type="button">Remove</button>
    </div>
    <div class="rec-grid">
      <div class="field"><label>From</label><input class="rfrom" type="time" value="${esc(rs.from)}"></div>
      <div class="field"><label>To</label><input class="rto" type="time" value="${esc(rs.to)}"></div>
      <div class="field"><label>Day of month</label><input class="rdom" type="number" min="1" max="31" value="${rs.day_of_month ?? 1}"></div>
      <div class="field"><label>Month</label><input class="rmonth" type="number" min="1" max="12" value="${rs.month ?? 1}"></div>
    </div>
    <div class="days">${dayChecks(rs.days || 0, "rday")}</div>
    <div class="rec-channels">${channels}</div>
  </div>`;
}

function renderRecurring() {
  $("#recurring").innerHTML = (draft.recurring || []).map(recRow).join("")
    || `<p class="hint">No recurring schedules yet.</p>`;
}

function collectChannels() {
  return $$("#schedules [data-channel]").map(row => ({
    id: Number(row.dataset.channel),
    name: $(".name", row).value.trim(),
    zone: $(".zone", row).value.trim(),
    equipment: $(".equipment", row).value,
    pulse_mode: $(".pulse", row).checked,
    pulse_on_ms: Number($(".pon", row).value),
    pulse_off_ms: Number($(".poff", row).value),
    enabled: $(".enabled", row).checked,
    days: readDays(row, "day"),
    mode: $(".mode", row).value,
    windows: readWindows(row)
  }));
}

function collectPump() {
  const prow = $("#pump-schedule");
  return {
    name: $(".pname", prow).value.trim(),
    enabled: $(".penabled", prow).checked,
    days: readDays(prow, "pday"),
    mode: $(".pmode", prow).value,
    windows: readWindows($(".pump-windows", prow)),
    min_off_sec: Number($(".pminoff", prow).value),
    max_run_min: Number($(".pmaxrun", prow).value),
    recovery_delay_sec: Number($(".precovery", prow).value),
    dry_run_current_a: Number($(".pdry", prow).value),
    dry_run_grace_sec: Number($(".pgrace", prow).value),
    resume_after_outage: $(".presume", prow).checked
  };
}

function collectRecurring() {
  return $$("#recurring [data-rec]").map(row => ({
    name: $(".rname", row).value.trim(),
    enabled: $(".renabled", row).checked,
    repeat: $(".rrepeat", row).value,
    days: readDays(row, "rday"),
    day_of_month: Number($(".rdom", row).value),
    month: Number($(".rmonth", row).value),
    from: $(".rfrom", row).value,
    to: $(".rto", row).value,
    channel_mask: $$(".rch", row).reduce((n, el) => n | (el.checked ? 1 << (Number(el.value) - 1) : 0), 0),
    channels: [],
    action: $(".raction", row).value
  }));
}

async function saveConfig() {
  try {
    const body = {
      profile: $("#profile-select").value,
      channels: collectChannels(),
      recurring: collectRecurring(),
      pump: collectPump()
    };
    await api(`/api/devices/${encodeURIComponent(deviceId)}/config`, {method: "POST", body: JSON.stringify(body)});
    if (me.role === "admin") {
      const interval = Number($("#interval-input").value);
      await api(`/api/devices/${encodeURIComponent(deviceId)}/update`, {
        method: "POST",
        body: JSON.stringify({profile: body.profile, poll_interval_sec: interval})
      });
    }
    draftDirty = false;
    editorsRendered = false;
    showToast("Configuration queued for the next CoAP poll");
    await loadDevice(false);
  } catch (e) { showToast(e.message, true); }
}

function renderActivity() {
  const history = (snap && snap.ackHistory) || [];
  $("#acks").innerHTML = history.length ? history.map(a =>
    `<div class="log-item"><span>${esc(a.at)}</span><b>${esc(a.result)}</b><span>${esc(a.id)}</span></div>`
  ).join("") : `<p class="hint">No acknowledgements yet.</p>`;

  const events = (snap && snap.events) || [];
  $("#events").innerHTML = events.length ? events.map(e =>
    `<div class="log-item"><span>${esc(e.at)}</span><b>${esc(e.type)}</b><span>${esc(e.message)}</span></div>`
  ).join("") : `<p class="hint">No events yet.</p>`;
}

// ---------------------------------------------------------------- actions
async function control(cmd, message) {
  try {
    await api(`/api/devices/${encodeURIComponent(deviceId)}/control`, {method: "POST", body: JSON.stringify(cmd)});
    showToast(message || "Control queued");
    await loadDevice(false);
  } catch (e) { showToast(e.message, true); }
}

async function confirmDevice(id) {
  try {
    await api(`/api/devices/${encodeURIComponent(id)}/confirm`, {method: "POST", body: JSON.stringify({})});
    showToast(`Device ${id} confirmed`);
    if (deviceId === id) await loadDevice(true); else await loadOverview();
  } catch (e) { showToast(e.message, true); }
}

async function rejectDevice(id) {
  try {
    await api(`/api/devices/${encodeURIComponent(id)}/reject`, {method: "POST", body: "{}"});
    showToast(`Device ${id} rejected`);
    if (deviceId === id) await loadDevice(true); else await loadOverview();
  } catch (e) { showToast(e.message, true); }
}

async function deleteDevice(id) {
  if (!confirm(`Delete device ${id} and its queued requests?`)) return;
  try {
    await api(`/api/devices/${encodeURIComponent(id)}`, {method: "DELETE"});
    showToast(`Device ${id} deleted`);
    if (deviceId === id) backToFleet(); else await loadOverview();
  } catch (e) { showToast(e.message, true); }
}

function switchTab(name) {
  activeTab = name;
  $$(".tab").forEach(t => t.classList.toggle("active", t.dataset.tab === name));
  $$(".tab-page").forEach(p => { p.hidden = p.id !== "tab-" + name; });
  if ((name === "sched" || name === "rec") && draft && (!editorsRendered || !draftDirty)) {
    if (!editorsRendered) renderEditors();
  }
  if (name === "activity") renderActivity();
}

// ------------------------------------------------------------------ events
document.addEventListener("click", async ev => {
  const t = ev.target;
  if (t.closest("#refresh") || t.closest("#adm-refresh")) { await refresh(); showToast("Refreshed"); return; }
  if (t.closest("#back-btn")) { backToFleet(); return; }
  if (t.closest("#dev-confirm")) { await confirmDevice(deviceId); return; }
  if (t.closest("#dev-reject")) { await rejectDevice(deviceId); return; }
  if (t.closest("#dev-delete")) { await deleteDevice(deviceId); return; }

  const confirmBtn = t.closest("[data-confirm]");
  if (confirmBtn) { ev.stopPropagation(); await confirmDevice(confirmBtn.dataset.confirm); return; }
  const rejectBtn = t.closest("[data-reject]");
  if (rejectBtn) { ev.stopPropagation(); await rejectDevice(rejectBtn.dataset.reject); return; }
  const openBtn = t.closest("[data-open]");
  if (openBtn) { ev.stopPropagation(); await openDevice(openBtn.dataset.open); return; }
  const card = t.closest(".device-card");
  if (card) { await openDevice(card.dataset.device); return; }

  const tab = t.closest(".tab");
  if (tab) { switchTab(tab.dataset.tab); return; }

  const relayBtn = t.closest("[data-relay]");
  if (relayBtn) {
    await control({target: "relay", id: Number(relayBtn.dataset.relay), mode: relayBtn.dataset.mode},
      `Relay ${relayBtn.dataset.relay} ${relayBtn.dataset.mode} queued`);
    return;
  }
  const pulseBtn = t.closest("[data-pulse]");
  if (pulseBtn) {
    await control({target: "relay", id: Number(pulseBtn.dataset.pulse), mode: "on", pulse_ms: Number(pulseBtn.dataset.ms)},
      `Relay ${pulseBtn.dataset.pulse} pulse queued`);
    return;
  }
  const pumpBtn = t.closest("[data-pump]");
  if (pumpBtn) {
    const mode = pumpBtn.dataset.pump;
    if (mode === "boost") await control({target: "pump", mode: "auto", boost_min: 30}, "Pump boost queued");
    else await control({target: "pump", mode}, `Pump ${mode} queued`);
    return;
  }
  if (t.closest("#request-data")) {
    try { await api(`/api/devices/${encodeURIComponent(deviceId)}/data-request`, {method: "POST"}); showToast("Data request queued"); await loadDevice(false); }
    catch (e) { showToast(e.message, true); }
    return;
  }
  if (t.closest("#all-auto")) { await control({target: "all", mode: "auto"}, "All-auto queued"); return; }
  if (t.closest("#all-off")) { await control({target: "all", mode: "off"}, "All-off queued"); return; }
  if (t.closest("#clear-queue")) {
    try { await api(`/api/devices/${encodeURIComponent(deviceId)}/queue`, {method: "DELETE"}); showToast("Queue cleared"); await loadDevice(false); }
    catch (e) { showToast(e.message, true); }
    return;
  }
  if (t.closest("#save-config") || t.closest("#save-config-rec")) { await saveConfig(); return; }
  if (t.closest("#add-rec")) {
    if (!draft) return;
    if ((draft.recurring || []).length >= 8) return showToast("Maximum 8 recurring schedules", true);
    draft.recurring = draft.recurring || [];
    draft.recurring.push({name: "New schedule", enabled: true, repeat: "daily", days: 127, day_of_month: 1, month: 1, from: "06:00", to: "07:00", channel_mask: 1, channels: [], action: "on"});
    renderRecurring();
    draftDirty = true;
    return;
  }
  const remRec = t.closest("[data-remrec]");
  if (remRec) { remRec.closest("[data-rec]").remove(); draftDirty = true; return; }

  const add = t.closest(".addwin");
  if (add) {
    const host = $(".windows", add.closest(".sched-row"));
    if ($$(".win", host).length >= 4) return showToast("Maximum 4 windows", true);
    host.insertAdjacentHTML("beforeend", windowRow());
    draftDirty = true;
    return;
  }
  const padd = t.closest(".paddwin");
  if (padd) {
    const host = $(".pump-windows");
    if ($$(".win", host).length >= 4) return showToast("Maximum 4 windows", true);
    host.insertAdjacentHTML("beforeend", windowRow());
    draftDirty = true;
    return;
  }
  const del = t.closest(".delwin");
  if (del) { del.closest(".win").remove(); draftDirty = true; }
});

document.addEventListener("input", ev => {
  if (ev.target.closest("#tab-sched") || ev.target.closest("#tab-rec")) draftDirty = true;
});
document.addEventListener("change", ev => {
  if (ev.target.closest("#tab-sched") || ev.target.closest("#tab-rec")) draftDirty = true;
});

initTheme();
$("#theme-toggle").addEventListener("click", () => {
  applyTheme(document.documentElement.dataset.theme === "light" ? "dark" : "light", true);
});
init();
