// MRAF Farm Controller
// 12 scheduled relays + one dedicated 5 HP 3-phase pump contactor
// ESP32 DevKit V1 (ESP32-WROOM-32), Arduino-ESP32 core
// Wi-Fi station with AP fallback, built-in HTTP server, NTP + DS3231 RTC.

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <time.h>
#include <sys/time.h>
#include <stdlib.h>

#include "config.h"
#include "model.h"
#include "settings.h"
#include "schedule.h"
#include "rtc.h"
#include "controller.h"
#include "coap_client.h"
#include "webui.h"

Config g_cfg;
Controller g_ctrl;
WebServer server(80);

bool g_apMode = false;
unsigned long g_bootMs = 0;
time_t g_lastTick = 0;
time_t g_lastRtcWrite = 0;
bool g_rtcOk = false;
float g_acsZeroMv = 1250.0f;       // ACS712 2.5 V zero / 2 (divider)
bool g_powerRaw = true;
bool g_powerState = true;
unsigned long g_powerChangeMs = 0;
unsigned long g_lastPowerPoll = 0;
unsigned long g_lastWifiRetry = 0;

// Remote CoAP poll state. The controller asks the server for a control or
// data request, applies/queues it, and reports the result on the next poll.
char g_remoteAckId[33] = "";
char g_remoteAckResult[24] = "";
bool g_remoteWantData = false;
char g_remoteLastId[33] = "";
char g_remoteLastResult[24] = "";
unsigned long g_remoteLastPollMs = 0;
time_t g_remoteLastPollAt = 0;
int g_remoteLastCode = 0;
bool g_remoteLastOk = false;
char g_remoteLastError[64] = "";
char g_remoteStatus[16] = "off";       // off|waiting|pending|confirmed|rejected|denied
bool g_remoteConfirmed = false;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static float readAdcMilliVolts(uint8_t pin, uint8_t samples) {
    uint32_t sum = 0;
    for (uint8_t i = 0; i < samples; i++) sum += analogReadMilliVolts(pin);
    return (float)sum / (float)samples;
}

static float readBatteryVolts() {
    float mv = readAdcMilliVolts(BATT_SENSE_PIN, 16);
    if (mv < 200.0f) return 0.0f;          // sensor not fitted
    return (mv / 1000.0f) * BATT_DIVIDER_RATIO;
}

static float readPumpCurrent() {
    if (!g_powerState) return 0.0f;
    float mv = readAdcMilliVolts(PUMP_CURRENT_PIN, 12);
    float delta = mv - g_acsZeroMv;
    if (delta < 0) delta = 0;
    // Starting current saturates the ACS712; that is acceptable, the running
    // current is what is used for dry-run and run feedback.
    return delta / ACS_SENSITIVITY_MV_PER_A;
}

static bool readFloat(uint8_t pin, bool activeLow) {
    int v = digitalRead(pin);
    return activeLow ? (v == LOW) : (v == HIGH);
}

static void pollMainsPresence() {
    bool raw = (digitalRead(MAINS_SENSE_PIN) == HIGH) == MAINS_SENSE_ACTIVE_HIGH;
    if (raw != g_powerRaw) {
        g_powerRaw = raw;
        g_powerChangeMs = millis();
    }
    if (millis() - g_powerChangeMs >= 800) g_powerState = g_powerRaw;
}

static void updateStatusLed() {
    bool on;
    if (g_apMode) {
        on = ((millis() / 250) % 2) == 0;                  // fast blink
    } else if (WiFi.status() == WL_CONNECTED) {
        on = ((millis() / 1000) % 2) == 0;                 // slow heartbeat
    } else {
        on = ((millis() / 120) % 2) == 0;                  // rapid = no link
    }
    digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW);
}

static void addWindows(JsonObject obj, const SchedWindow *win, uint8_t n) {
    JsonArray arr = obj["windows"].to<JsonArray>();
    for (uint8_t i = 0; i < n; i++) {
        JsonObject w = arr.add<JsonObject>();
        char on[6], off[6];
        minutesToHHMM(win[i].on_min, on, sizeof(on));
        minutesToHHMM(win[i].off_min, off, sizeof(off));
        w["on"] = on;
        w["off"] = off;
    }
}

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------
static bool requireAuth() {
    if (server.authenticate(g_cfg.auth_user, g_cfg.auth_pass)) return true;
    server.requestAuthentication(BASIC_AUTH, "MRAF", "Login required");
    return false;
}

static void sendJson(JsonDocument &doc, int code = 200) {
    String out;
    serializeJson(doc, out);
    server.sendHeader("Cache-Control", "no-store");
    server.send(code, "application/json", out);
}

static void sendError(int code, const char *msg) {
    JsonDocument d;
    d["ok"] = false;
    d["error"] = msg;
    sendJson(d, code);
}

static void buildStatus(JsonDocument &doc) {
    time_t now = time(nullptr);

    doc["simulator"] = false;
    doc["fw"] = FW_VERSION;
    doc["device"] = g_cfg.device_name;
    doc["now"] = (uint32_t)now;
    doc["now_local"] = localTimeString(now);
    doc["time_valid"] = now > 1700000000;
    doc["uptime_sec"] = (uint32_t)((millis() - g_bootMs) / 1000UL);

    JsonObject w = doc["wifi"].to<JsonObject>();
    w["mode"] = g_apMode ? "ap" : "sta";
    w["ssid"] = g_apMode ? String(g_cfg.device_name) : WiFi.SSID();
    w["ip"] = g_apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    w["rssi"] = g_apMode ? 0 : WiFi.RSSI();
    w["hostname"] = g_cfg.hostname;
    w["connected"] = g_apMode ? true : (WiFi.status() == WL_CONNECTED);

    JsonObject rem = doc["remote"].to<JsonObject>();
    rem["enabled"] = g_cfg.coap_enabled;
    rem["host"] = g_cfg.coap_host;
    rem["port"] = g_cfg.coap_port;
    rem["path"] = g_cfg.coap_path;
    rem["interval_sec"] = g_cfg.coap_interval_sec;
    rem["last_code"] = g_remoteLastCode;
    rem["last_ok"] = g_remoteLastOk;
    rem["last_error"] = g_remoteLastError;
    rem["last_poll"] = (uint32_t)g_remoteLastPollAt;
    rem["pending"] = (g_remoteAckId[0] != '\0' || g_remoteWantData);
    rem["status"] = g_remoteStatus;
    rem["confirmed"] = g_remoteConfirmed;
    rem["device_id"] = g_cfg.device_id;
    rem["user"] = g_cfg.remote_user;

    doc["profile"] = profileName(g_cfg.profile);

    JsonObject pw = doc["power"].to<JsonObject>();
    pw["present"] = g_ctrl.powerPresent();
    pw["battery_v"] = g_ctrl.batteryVoltage();
    pw["outage_sec"] = (uint32_t)g_ctrl.outageSeconds(now);
    pw["battery_backed"] = true;

    JsonObject s = doc["sensors"].to<JsonObject>();
    s["tank_full"] = g_ctrl.pump().tank_full;
    s["sump_dry"] = g_ctrl.pump().sump_dry;
    s["acs_zero_mv"] = g_acsZeroMv;

    JsonArray relays = doc["relays"].to<JsonArray>();
    for (uint8_t i = 0; i < MAX_CHANNELS; i++) {
        const ChannelConfig &ch = g_ctrl.config().channels[i];
        const RelayRuntime &rt = g_ctrl.relay(i);
        JsonObject o = relays.add<JsonObject>();
        o["id"] = i + 1;
        o["name"] = ch.name;
        o["zone"] = ch.zone;
        o["equipment"] = equipmentName(ch.equipment);
        o["pulse_mode"] = ch.pulse_mode;
        o["pulse_on_ms"] = ch.pulse_on_ms;
        o["pulse_off_ms"] = ch.pulse_off_ms;
        o["enabled"] = ch.enabled;
        o["days"] = ch.days;
        o["mode"] = modeName(ch.mode);
        o["on"] = rt.on;
        o["output"] = rt.output;
        o["pulsing"] = rt.pulsing;
        o["schedule"] = rt.schedule_active;
        o["recurring"] = rt.recurring_active;
        o["boost_until"] = (uint32_t)rt.boost_until;
        o["last_change"] = (uint32_t)rt.last_change;
        addWindows(o, ch.win, ch.nwin);
    }

    const PumpConfig &p = g_ctrl.config().pump;
    const PumpRuntime &pr = g_ctrl.pump();
    JsonObject pj = doc["pump"].to<JsonObject>();
    pj["name"] = p.name;
    pj["enabled"] = p.enabled;
    pj["days"] = p.days;
    pj["mode"] = modeName(p.mode);
    pj["on"] = pr.on;
    pj["want"] = pr.want;
    pj["schedule"] = pr.schedule_active;
    pj["boost_until"] = (uint32_t)pr.boost_until;
    pj["state"] = g_ctrl.pumpStateName(now);
    pj["fault"] = pr.fault;
    pj["fault_latched"] = pr.fault_latched;
    pj["current_a"] = pr.current_a;
    pj["run_sec"] = pr.on ? (uint32_t)(now - pr.run_start) : 0;
    pj["last_off"] = (uint32_t)pr.last_off;
    pj["last_change"] = (uint32_t)pr.last_change;
    pj["external_run"] = pr.external_run;
    pj["tank_full"] = pr.tank_full;
    pj["sump_dry"] = pr.sump_dry;
    pj["min_off_sec"] = p.min_off_sec;
    pj["max_run_min"] = p.max_run_min;
    pj["recovery_delay_sec"] = p.recovery_delay_sec;
    pj["resume_after_outage"] = p.resume_after_outage;
    pj["dry_run_current_a"] = p.dry_run_current_a;
    pj["dry_run_grace_sec"] = p.dry_run_grace_sec;
    addWindows(pj, p.win, p.nwin);

    JsonArray ev = doc["events"].to<JsonArray>();
    uint8_t count = g_ctrl.eventCount();
    uint8_t start = count > 30 ? (uint8_t)(count - 30) : 0;
    for (uint8_t i = start; i < count; i++) {
        const EventRecord &e = g_ctrl.event(i);
        JsonObject o = ev.add<JsonObject>();
        o["t"] = (uint32_t)e.t;
        o["time"] = localTimeString(e.t);
        o["src"] = e.src;
        o["msg"] = e.msg;
    }
}

// ---------------------------------------------------------------------------
// HTTP handlers
// ---------------------------------------------------------------------------
static void handleRoot() {
    if (!requireAuth()) return;
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html", INDEX_HTML);
}

static void handleHealth() {
    JsonDocument d;
    d["ok"] = true;
    d["device"] = "mraf";
    d["fw"] = FW_VERSION;
    sendJson(d);
}

static void handleStatus() {
    if (!requireAuth()) return;
    JsonDocument d;
    buildStatus(d);
    sendJson(d);
}

static void handleConfigGet() {
    if (!requireAuth()) return;
    JsonDocument d;
    settingsToJson(g_ctrl.config(), d, false);
    sendJson(d);
}

static void handleConfigPost() {
    if (!requireAuth()) return;
    if (!server.hasArg("plain")) {
        sendError(400, "missing JSON body");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
        sendError(400, err.c_str());
        return;
    }

    Config old = g_cfg;
    String error;
    if (!settingsFromJson(doc, g_cfg, error)) {
        g_cfg = old;
        sendError(400, error.c_str());
        return;
    }

    bool reboot = strcmp(old.wifi_ssid, g_cfg.wifi_ssid) != 0 ||
                  strcmp(old.wifi_pass, g_cfg.wifi_pass) != 0 ||
                  strcmp(old.hostname, g_cfg.hostname) != 0 ||
                  strcmp(old.auth_user, g_cfg.auth_user) != 0 ||
                  strcmp(old.auth_pass, g_cfg.auth_pass) != 0;

    settingsSave(g_cfg);
    g_ctrl.applyConfig(g_cfg);
    setenv("TZ", g_cfg.timezone, 1);
    tzset();

    JsonDocument out;
    settingsToJson(g_cfg, out, false);
    out["ok"] = true;
    out["reboot_required"] = reboot;
    sendJson(out);

    if (reboot) {
        delay(600);
        ESP.restart();
    }
}

static bool applyControlCommand(JsonObject cmd, time_t now, String &error) {
    const char *target = cmd["target"] | "";
    const char *modeStr = cmd["mode"] | "auto";
    bool ok = false;
    RunMode mode = modeFromString(modeStr, ok);
    if (!ok) {
        error = "mode must be auto/on/off";
        return false;
    }
    int boost_i = cmd["boost_min"] | 0;
    if (boost_i < 0) boost_i = 0;
    uint16_t boost = (uint16_t)boost_i;
    int pulse_i = cmd["pulse_ms"] | 0;
    if (pulse_i < 0) pulse_i = 0;
    if (pulse_i > 10000) {
        error = "pulse_ms must be 0..10000";
        return false;
    }
    uint16_t pulse_ms = (uint16_t)pulse_i;

    if (strcmp(target, "relay") == 0) {
        int id = cmd["id"] | 0;
        if (id < 1 || id > MAX_CHANNELS) {
            error = "relay id must be 1..12";
            return false;
        }
        g_ctrl.setRelayMode((uint8_t)(id - 1), mode, boost, now, pulse_ms);
    } else if (strcmp(target, "pump") == 0) {
        g_ctrl.setPumpMode(mode, boost, now);
    } else if (strcmp(target, "all") == 0) {
        if (mode == MODE_OFF) g_ctrl.allOff(now);
        else if (mode == MODE_AUTO) g_ctrl.allAuto(now);
        else {
            error = "all supports auto/off only";
            return false;
        }
    } else {
        error = "target must be relay/pump/all";
        return false;
    }
    return true;
}

static void handleControl() {
    if (!requireAuth()) return;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
        sendError(400, err.c_str());
        return;
    }

    time_t now = time(nullptr);
    String error;
    if (!applyControlCommand(doc.as<JsonObject>(), now, error)) {
        sendError(400, error.c_str());
        return;
    }

    g_cfg = g_ctrl.config();
    settingsSave(g_ctrl.config());

    JsonDocument out;
    buildStatus(out);
    out["ok"] = true;
    sendJson(out);
}

static void copyRemoteText(char *dst, size_t n, const char *src) {
    if (!dst || n == 0) return;
    if (!src) src = "";
    strncpy(dst, src, n - 1);
    dst[n - 1] = '\0';
}

static void setRemoteAck(const char *id, const char *result) {
    copyRemoteText(g_remoteAckId, sizeof(g_remoteAckId), id);
    copyRemoteText(g_remoteAckResult, sizeof(g_remoteAckResult), result);
}

static void buildRemoteData(JsonObject data, time_t now) {
    data["now"] = (uint32_t)now;
    data["power"] = g_ctrl.powerPresent();
    data["battery_v"] = g_ctrl.batteryVoltage();
    data["tank_full"] = g_ctrl.pump().tank_full;
    data["sump_dry"] = g_ctrl.pump().sump_dry;
    data["wifi_rssi"] = g_apMode ? 0 : WiFi.RSSI();
    data["ip"] = g_apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    data["profile"] = profileName(g_cfg.profile);
    data["confirmed"] = g_remoteConfirmed;

    JsonObject p = data["pump"].to<JsonObject>();
    p["on"] = g_ctrl.pump().on;
    p["state"] = g_ctrl.pumpStateName(now);
    p["mode"] = modeName(g_ctrl.config().pump.mode);
    p["current_a"] = g_ctrl.pump().current_a;
    p["fault"] = g_ctrl.pump().fault;
    p["fault_latched"] = g_ctrl.pump().fault_latched;

    JsonArray relays = data["relays"].to<JsonArray>();
    for (uint8_t i = 0; i < MAX_CHANNELS; i++)
        relays.add(g_ctrl.relay(i).on ? 1 : 0);

    JsonArray equipment = data["equipment"].to<JsonArray>();
    for (uint8_t i = 0; i < MAX_CHANNELS; i++) {
        const ChannelConfig &ch = g_ctrl.config().channels[i];
        JsonObject e = equipment.add<JsonObject>();
        e["id"] = i + 1;
        e["kind"] = equipmentName(ch.equipment);
        e["zone"] = ch.zone;
        e["on"] = g_ctrl.relay(i).on;
        e["output"] = g_ctrl.relay(i).output;
        e["pulsing"] = g_ctrl.relay(i).pulsing;
    }
}

// Apply the server-controlled metadata that may accompany any poll response:
// confirmation state, profile and poll interval. The server owns the polling
// schedule, so a changed interval is persisted to NVS immediately.
static void applyRemoteMeta(JsonObject res, time_t now) {
    bool changed = false;

    if (!res["confirmed"].isNull()) {
        bool confirmed = res["confirmed"].as<bool>();
        if (confirmed != g_remoteConfirmed) {
            g_remoteConfirmed = confirmed;
            copyRemoteText(g_remoteStatus, sizeof(g_remoteStatus),
                           confirmed ? "confirmed" : "pending");
            g_ctrl.addEvent("remote", confirmed ? "device confirmed" :
                            "waiting for admin", now);
            changed = true;
        }
    }

    const char *profile = res["profile"] | "";
    if (profile[0]) {
        uint8_t p = g_cfg.profile;
        if (strcmp(profile, "agri") == 0) p = PROFILE_AGRI;
        else if (strcmp(profile, "home") == 0) p = PROFILE_HOME;
        else if (strcmp(profile, "godown") == 0) p = PROFILE_GODOWN;
        else if (strcmp(profile, "industry") == 0) p = PROFILE_INDUSTRY;
        if (p != g_cfg.profile) {
            g_cfg.profile = p;
            g_ctrl.applyConfig(g_cfg);
            g_ctrl.addEvent("remote", "profile updated by server", now);
            changed = true;
        }
    }

    int interval = res["poll_interval_sec"] | 0;
    if (interval >= 10 && interval <= 3600 &&
        (uint16_t)interval != g_cfg.coap_interval_sec) {
        g_cfg.coap_interval_sec = (uint16_t)interval;
        g_ctrl.applyConfig(g_cfg);
        g_ctrl.addEvent("remote", "poll interval updated by server", now);
        changed = true;
    }

    if (changed) settingsSave(g_cfg);
}

static void remotePoll() {
    time_t now = time(nullptr);
    g_remoteLastPollAt = now;
    if (g_remoteStatus[0] == '\0' || strcmp(g_remoteStatus, "off") == 0)
        copyRemoteText(g_remoteStatus, sizeof(g_remoteStatus), "waiting");

    JsonDocument req;
    req["device"] = g_cfg.device_name;
    req["device_id"] = g_cfg.device_id;
    req["fw"] = FW_VERSION;
    req["token"] = g_cfg.coap_token;
    req["time"] = (uint32_t)now;
    req["profile"] = profileName(g_cfg.profile);
    req["confirmed"] = g_remoteConfirmed;
    req["register"] = g_cfg.remote_register;
    req["poll_interval_sec"] = g_cfg.coap_interval_sec;
    JsonObject auth = req["auth"].to<JsonObject>();
    auth["user"] = g_cfg.remote_user;
    auth["pass"] = g_cfg.remote_pass;
    if (g_remoteAckId[0]) {
        req["ack"] = g_remoteAckId;
        req["result"] = g_remoteAckResult;
    }
    if (g_remoteWantData) {
        JsonObject data = req["data"].to<JsonObject>();
        buildRemoteData(data, now);
    }

    String body;
    serializeJson(req, body);

    static char resp[8192];
    int code = coapPost(g_cfg.coap_host, g_cfg.coap_port, g_cfg.coap_path,
                        body.c_str(), resp, sizeof(resp), 900);
    g_remoteLastCode = code;
    if (code < 64 || code >= 96) {
        g_remoteLastOk = false;
        snprintf(g_remoteLastError, sizeof(g_remoteLastError),
                 "CoAP error %d", code);
        return;
    }

    g_remoteLastOk = true;
    g_remoteLastError[0] = '\0';

    // The previous request has been acknowledged. Queue a new request only
    // after the exchange succeeds, so a lost response does not drop the ack.
    g_remoteAckId[0] = '\0';
    g_remoteAckResult[0] = '\0';
    g_remoteWantData = false;

    if (!resp[0]) return;

    JsonDocument res;
    DeserializationError err = deserializeJson(res, resp);
    if (err) {
        g_remoteLastOk = false;
        copyRemoteText(g_remoteLastError, sizeof(g_remoteLastError),
                       "invalid CoAP JSON");
        return;
    }

    const char *rq = res["request"] | "";
    const char *id = res["id"] | "";

    applyRemoteMeta(res.as<JsonObject>(), now);

    // Registration handshake. Until an admin confirms the device the server
    // only returns register/pending; no control or configuration is accepted.
    if (strcmp(rq, "register") == 0) {
        const char *status = res["status"] | "pending";
        if (strcmp(status, "rejected") == 0) {
            copyRemoteText(g_remoteStatus, sizeof(g_remoteStatus), "rejected");
            g_remoteConfirmed = false;
            copyRemoteText(g_remoteLastError, sizeof(g_remoteLastError),
                           res["message"] | "registration rejected");
        } else if (strcmp(status, "confirmed") == 0) {
            copyRemoteText(g_remoteStatus, sizeof(g_remoteStatus), "confirmed");
            g_remoteConfirmed = true;
        } else if (strcmp(status, "credentials_required") == 0) {
            copyRemoteText(g_remoteStatus, sizeof(g_remoteStatus), "denied");
            copyRemoteText(g_remoteLastError, sizeof(g_remoteLastError),
                           "server requires device credentials");
        } else {
            copyRemoteText(g_remoteStatus, sizeof(g_remoteStatus), "pending");
            copyRemoteText(g_remoteLastError, sizeof(g_remoteLastError),
                           res["message"] | "waiting for admin confirmation");
        }
        return;
    }
    if (strcmp(rq, "auth") == 0) {
        copyRemoteText(g_remoteStatus, sizeof(g_remoteStatus), "denied");
        g_remoteConfirmed = false;
        copyRemoteText(g_remoteLastError, sizeof(g_remoteLastError),
                       res["message"] | "device credentials rejected");
        return;
    }

    if (strcmp(rq, "control") == 0) {
        if (id[0] && strcmp(id, g_remoteLastId) == 0) {
            setRemoteAck(id, g_remoteLastResult);
            return;
        }

        int applied = 0;
        int failed = 0;
        String error;
        JsonArray cmds = res["commands"].as<JsonArray>();
        for (JsonVariant v : cmds) {
            if (!v.is<JsonObject>()) {
                failed++;
                continue;
            }
            if (applyControlCommand(v.as<JsonObject>(), now, error)) applied++;
            else failed++;
        }

        if (applied > 0) {
            g_cfg = g_ctrl.config();
            settingsSave(g_cfg);
        }

        const char *result = (applied > 0 && failed == 0) ? "applied" :
                             (applied > 0 ? "partial" : "rejected");
        if (id[0]) {
            copyRemoteText(g_remoteLastId, sizeof(g_remoteLastId), id);
            copyRemoteText(g_remoteLastResult, sizeof(g_remoteLastResult), result);
        }
        setRemoteAck(id, result);
    } else if (strcmp(rq, "data") == 0) {
        setRemoteAck(id, "data");
        g_remoteWantData = true;
    } else if (strcmp(rq, "config") == 0) {
        if (id[0] && strcmp(id, g_remoteLastId) == 0) {
            setRemoteAck(id, g_remoteLastResult);
            return;
        }

        String error;
        bool ok = settingsFromJson(res, g_cfg, error);
        const char *result = "rejected";
        if (ok) {
            g_ctrl.applyConfig(g_cfg);
            settingsSave(g_cfg);
            result = "applied";
        } else {
            copyRemoteText(g_remoteLastError, sizeof(g_remoteLastError),
                           error.c_str());
        }
        if (id[0]) {
            copyRemoteText(g_remoteLastId, sizeof(g_remoteLastId), id);
            copyRemoteText(g_remoteLastResult, sizeof(g_remoteLastResult), result);
        }
        setRemoteAck(id, result);
    } else if (strcmp(rq, "none") != 0 && rq[0]) {
        copyRemoteText(g_remoteLastError, sizeof(g_remoteLastError),
                       "unknown remote request");
    }
}

static void handlePumpReset() {
    if (!requireAuth()) return;
    g_ctrl.resetPumpFault(time(nullptr));
    JsonDocument out;
    buildStatus(out);
    out["ok"] = true;
    sendJson(out);
}

static void handleTime() {
    if (!requireAuth()) return;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
        sendError(400, err.c_str());
        return;
    }
    uint64_t epoch = doc["epoch"].as<uint64_t>();
    if (epoch < 1700000000ULL) {
        sendError(400, "epoch must be a Unix timestamp");
        return;
    }
    struct timeval tv;
    tv.tv_sec = (time_t)epoch;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
    if (g_rtcOk) rtcWriteLocal((time_t)epoch);

    JsonDocument out;
    buildStatus(out);
    out["ok"] = true;
    sendJson(out);
}

static void handleEvents() {
    if (!requireAuth()) return;
    JsonDocument d;
    JsonArray ev = d["events"].to<JsonArray>();
    uint8_t count = g_ctrl.eventCount();
    for (uint8_t i = 0; i < count; i++) {
        const EventRecord &e = g_ctrl.event(i);
        JsonObject o = ev.add<JsonObject>();
        o["t"] = (uint32_t)e.t;
        o["time"] = localTimeString(e.t);
        o["src"] = e.src;
        o["msg"] = e.msg;
    }
    sendJson(d);
}

static void handleScan() {
    if (!requireAuth()) return;
    int n = WiFi.scanNetworks();
    JsonDocument d;
    JsonArray arr = d["networks"].to<JsonArray>();
    for (int i = 0; i < n; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["ssid"] = WiFi.SSID(i);
        o["rssi"] = WiFi.RSSI(i);
        o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    }
    WiFi.scanDelete();
    sendJson(d);
}

static void handleReboot() {
    if (!requireAuth()) return;
    JsonDocument d;
    d["ok"] = true;
    d["reboot"] = true;
    sendJson(d);
    delay(500);
    ESP.restart();
}

static void handleNotFound() {
    if (!requireAuth()) return;
    sendError(404, "not found");
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(100);
    g_bootMs = millis();

    settingsDefaults(g_cfg);
    settingsLoad(g_cfg);

    // A stable device identity is required for the server registration
    // handshake. Derive it from the factory MAC on first boot and persist it.
    if (g_cfg.device_id[0] == '\0') {
        uint64_t mac = ESP.getEfuseMac();
        snprintf(g_cfg.device_id, sizeof(g_cfg.device_id), "ESP32-%06X",
                 (unsigned)(mac & 0xFFFFFF));
        settingsSave(g_cfg);
    }
    if (g_cfg.coap_enabled)
        copyRemoteText(g_remoteStatus, sizeof(g_remoteStatus), "waiting");

    setenv("TZ", g_cfg.timezone, 1);
    tzset();

    g_ctrl.begin(g_cfg);

    pinMode(MAINS_SENSE_PIN, INPUT);
    pinMode(TANK_FULL_PIN, INPUT);
    pinMode(SUMP_DRY_PIN, INPUT);
    pinMode(BATT_SENSE_PIN, INPUT);
    pinMode(PUMP_CURRENT_PIN, INPUT);
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, LOW);
    analogReadResolution(12);

    g_rtcOk = rtcBegin();
    time_t rtcTime;
    if (g_rtcOk && rtcReadLocal(rtcTime)) {
        struct timeval tv;
        tv.tv_sec = rtcTime;
        tv.tv_usec = 0;
        settimeofday(&tv, nullptr);
        Serial.printf("RTC time: %s\n", localTimeString(rtcTime).c_str());
    } else {
        Serial.println("RTC not present or not set; waiting for NTP/manual time");
    }

    // Calibrate the ACS712 zero only if the reading looks like a healthy
    // zero-current output. The pump must be OFF during boot for best results.
    delay(250);
    float zero = readAdcMilliVolts(PUMP_CURRENT_PIN, 32);
    if (zero > 800.0f && zero < 1800.0f) g_acsZeroMv = zero;
    Serial.printf("ACS712 zero: %.1f mV\n", g_acsZeroMv);

    g_powerRaw = g_powerState =
        (digitalRead(MAINS_SENSE_PIN) == HIGH) == MAINS_SENSE_ACTIVE_HIGH;
    g_powerChangeMs = millis();

    WiFi.setSleep(false);
    if (strlen(g_cfg.wifi_ssid) > 0) {
        WiFi.mode(WIFI_STA);
        WiFi.setHostname(g_cfg.hostname);
        WiFi.begin(g_cfg.wifi_ssid, g_cfg.wifi_pass);
        Serial.printf("Connecting to Wi-Fi '%s'", g_cfg.wifi_ssid);
        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 20000UL) {
            delay(250);
            Serial.print(".");
        }
        Serial.println();
    }

    if (WiFi.status() == WL_CONNECTED) {
        g_apMode = false;
        Serial.print("Wi-Fi connected: ");
        Serial.println(WiFi.localIP());
    } else {
        g_apMode = true;
        char apSsid[32];
        snprintf(apSsid, sizeof(apSsid), "MRAF-%06X",
                 (unsigned)(ESP.getEfuseMac() & 0xFFFFFF));
        WiFi.mode(WIFI_AP);
        WiFi.softAP(apSsid, DEFAULT_AP_PASS);
        Serial.printf("AP mode: %s / %s  http://%s\n", apSsid, DEFAULT_AP_PASS,
                      WiFi.softAPIP().toString().c_str());
    }

    if (MDNS.begin(g_cfg.hostname)) {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("mDNS: http://%s.local\n", g_cfg.hostname);
    }

    configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);

    g_ctrl.addEvent("system", "boot", time(nullptr));

    server.on("/", HTTP_GET, handleRoot);
    server.on("/health", HTTP_GET, handleHealth);
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/config", HTTP_GET, handleConfigGet);
    server.on("/api/config", HTTP_POST, handleConfigPost);
    server.on("/api/control", HTTP_POST, handleControl);
    server.on("/api/pump/reset", HTTP_POST, handlePumpReset);
    server.on("/api/time", HTTP_POST, handleTime);
    server.on("/api/events", HTTP_GET, handleEvents);
    server.on("/api/wifi/scan", HTTP_GET, handleScan);
    server.on("/api/reboot", HTTP_POST, handleReboot);
    server.onNotFound(handleNotFound);
    server.begin();
    Serial.println("HTTP server started on port 80");
}

void loop() {
    server.handleClient();

    unsigned long nowMs = millis();
    if (nowMs - g_lastPowerPoll >= 50UL) {
        g_lastPowerPoll = nowMs;
        pollMainsPresence();
    }

    time_t now = time(nullptr);
    if (now != g_lastTick) {
        g_lastTick = now;

        bool tankFull = readFloat(TANK_FULL_PIN, g_cfg.pump.tank_full_active_low);
        bool sumpDry = readFloat(SUMP_DRY_PIN, g_cfg.pump.sump_dry_active_low);
        float current = readPumpCurrent();
        float battery = readBatteryVolts();

        g_ctrl.tick(now, g_powerState, tankFull, sumpDry, current, battery);

        if (g_rtcOk && now > 1700000000 &&
            (g_lastRtcWrite == 0 || now - g_lastRtcWrite >= 3600)) {
            rtcWriteLocal(now);
            g_lastRtcWrite = now;
        }
    }

    updateStatusLed();

    // Momentary motor/contactor pulses are millisecond timers, so service
    // them on every loop rather than on the one-second control tick.
    g_ctrl.setPulseClock(millis());
    g_ctrl.servicePulses();

    // Poll the remote Java server over CoAP. The poll is skipped in AP mode
    // and while the station link is down.
    if (g_cfg.coap_enabled && !g_apMode && g_cfg.coap_host[0] &&
        WiFi.status() == WL_CONNECTED) {
        unsigned long interval = (unsigned long)g_cfg.coap_interval_sec * 1000UL;
        if (g_remoteLastPollMs == 0 ||
            nowMs - g_remoteLastPollMs >= interval) {
            g_remoteLastPollMs = nowMs;
            remotePoll();
        }
    }

    // Reconnect Wi-Fi every 30 s if the station link drops.
    if (!g_apMode && WiFi.status() != WL_CONNECTED &&
        millis() - g_lastWifiRetry >= 30000UL) {
        g_lastWifiRetry = millis();
        WiFi.reconnect();
    }

    delay(2);
    yield();
}
