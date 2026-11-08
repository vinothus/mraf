#include "settings.h"

#include <Preferences.h>
#include <string.h>
#include <time.h>

static void copyStr(char *dst, size_t n, const char *src) {
    if (!dst || n == 0) return;
    if (!src) src = "";
    strncpy(dst, src, n - 1);
    dst[n - 1] = '\0';
}

static void addWindow(ChannelConfig &ch, uint16_t on, uint16_t off) {
    if (ch.nwin >= MAX_WINDOWS) return;
    ch.win[ch.nwin].on_min = on;
    ch.win[ch.nwin].off_min = off;
    ch.nwin++;
}

static void addWindow(PumpConfig &p, uint16_t on, uint16_t off) {
    if (p.nwin >= MAX_WINDOWS) return;
    p.win[p.nwin].on_min = on;
    p.win[p.nwin].off_min = off;
    p.nwin++;
}

void settingsDefaults(Config &cfg) {
    memset(&cfg, 0, sizeof(cfg));

    copyStr(cfg.device_name, sizeof(cfg.device_name), DEFAULT_DEVICE_NAME);
    copyStr(cfg.timezone, sizeof(cfg.timezone), DEFAULT_TIMEZONE);
    copyStr(cfg.auth_user, sizeof(cfg.auth_user), DEFAULT_AUTH_USER);
    copyStr(cfg.auth_pass, sizeof(cfg.auth_pass), DEFAULT_AUTH_PASS);
    copyStr(cfg.device_id, sizeof(cfg.device_id), "");   // filled from MAC at boot
    cfg.profile = PROFILE_AGRI;

    copyStr(cfg.wifi_ssid, sizeof(cfg.wifi_ssid), "");
    copyStr(cfg.wifi_pass, sizeof(cfg.wifi_pass), "");
    copyStr(cfg.hostname, sizeof(cfg.hostname), DEFAULT_HOSTNAME);
    cfg.coap_enabled = false;
    copyStr(cfg.coap_path, sizeof(cfg.coap_path), DEFAULT_COAP_PATH);
    cfg.coap_port = DEFAULT_COAP_PORT;
    cfg.coap_interval_sec = DEFAULT_COAP_INTERVAL_SEC;
    copyStr(cfg.remote_user, sizeof(cfg.remote_user), DEFAULT_REMOTE_USER);
    copyStr(cfg.remote_pass, sizeof(cfg.remote_pass), DEFAULT_REMOTE_PASS);
    cfg.remote_register = true;
    cfg.max_boost_min = DEFAULT_MAX_BOOST_MIN;
    cfg.nrecurring = 0;

    for (uint8_t i = 0; i < MAX_CHANNELS; i++) {
        ChannelConfig &ch = cfg.channels[i];
        snprintf(ch.name, sizeof(ch.name), "Relay %u", (unsigned)(i + 1));
        snprintf(ch.zone, sizeof(ch.zone), "Zone %u", (unsigned)(i + 1));
        ch.equipment = EQ_OTHER;
        ch.pulse_mode = false;
        ch.pulse_on_ms = DEFAULT_PULSE_ON_MS;
        ch.pulse_off_ms = DEFAULT_PULSE_OFF_MS;
        ch.enabled = false;
        ch.days = 0;
        ch.nwin = 0;
        ch.mode = MODE_AUTO;
    }

    // Practical starting points for a mixed farm. Edit from the web UI.
    ChannelConfig &c1 = cfg.channels[0];
    copyStr(c1.name, sizeof(c1.name), "Shed Light");
    copyStr(c1.zone, sizeof(c1.zone), "Zone A");
    c1.equipment = EQ_LIGHT;
    c1.enabled = true; c1.days = 0x7F; c1.mode = MODE_AUTO;
    addWindow(c1, 18 * 60, 22 * 60);

    ChannelConfig &c2 = cfg.channels[1];
    copyStr(c2.name, sizeof(c2.name), "Yard Light");
    copyStr(c2.zone, sizeof(c2.zone), "Zone A");
    c2.equipment = EQ_LIGHT;
    c2.enabled = true; c2.days = 0x7F; c2.mode = MODE_AUTO;
    addWindow(c2, 18 * 60 + 30, 5 * 60 + 30);   // crosses midnight

    ChannelConfig &c3 = cfg.channels[2];
    copyStr(c3.name, sizeof(c3.name), "Drip Zone A");
    copyStr(c3.zone, sizeof(c3.zone), "Zone B");
    c3.equipment = EQ_VALVE;
    c3.enabled = true; c3.days = 0x15; c3.mode = MODE_AUTO;  // Mon/Wed/Fri
    addWindow(c3, 6 * 60, 6 * 60 + 30);

    ChannelConfig &c4 = cfg.channels[3];
    copyStr(c4.name, sizeof(c4.name), "Drip Zone B");
    copyStr(c4.zone, sizeof(c4.zone), "Zone C");
    c4.equipment = EQ_VALVE;
    c4.enabled = true; c4.days = 0x15; c4.mode = MODE_AUTO;
    addWindow(c4, 6 * 60 + 30, 7 * 60);

    ChannelConfig &c5 = cfg.channels[4];
    copyStr(c5.name, sizeof(c5.name), "Aerator 1");
    copyStr(c5.zone, sizeof(c5.zone), "Pond");
    c5.equipment = EQ_MOTOR;
    c5.pulse_mode = true;
    c5.enabled = true; c5.days = 0x7F; c5.mode = MODE_AUTO;
    addWindow(c5, 22 * 60, 4 * 60);             // crosses midnight

    ChannelConfig &c6 = cfg.channels[5];
    copyStr(c6.name, sizeof(c6.name), "Aerator 2");
    copyStr(c6.zone, sizeof(c6.zone), "Pond");
    c6.equipment = EQ_MOTOR;
    c6.pulse_mode = true;
    c6.enabled = true; c6.days = 0x7F; c6.mode = MODE_AUTO;
    addWindow(c6, 22 * 60, 4 * 60);

    ChannelConfig &c7 = cfg.channels[6];
    copyStr(c7.name, sizeof(c7.name), "Cattle Fan");
    copyStr(c7.zone, sizeof(c7.zone), "Shed");
    c7.equipment = EQ_FAN;
    c7.enabled = true; c7.days = 0x7F; c7.mode = MODE_AUTO;
    addWindow(c7, 10 * 60, 16 * 60);

    ChannelConfig &c8 = cfg.channels[7];
    copyStr(c8.name, sizeof(c8.name), "Milking Light");
    copyStr(c8.zone, sizeof(c8.zone), "Shed");
    c8.equipment = EQ_LIGHT;
    c8.enabled = true; c8.days = 0x7F; c8.mode = MODE_AUTO;
    addWindow(c8, 4 * 60 + 30, 6 * 60);

    ChannelConfig &c9 = cfg.channels[8];
    copyStr(c9.name, sizeof(c9.name), "Gate Light");
    copyStr(c9.zone, sizeof(c9.zone), "Gate");
    c9.equipment = EQ_LIGHT;
    c9.enabled = true; c9.days = 0x7F; c9.mode = MODE_AUTO;
    addWindow(c9, 19 * 60, 6 * 60);

    copyStr(cfg.channels[9].name, sizeof(cfg.channels[9].name), "Spare 1");
    copyStr(cfg.channels[10].name, sizeof(cfg.channels[10].name), "Spare 2");
    copyStr(cfg.channels[11].name, sizeof(cfg.channels[11].name), "Spare 3");

    PumpConfig &p = cfg.pump;
    copyStr(p.name, sizeof(p.name), "5HP Borewell Pump");
    p.enabled = true;
    p.days = 0x7F;
    p.mode = MODE_AUTO;
    p.nwin = 0;
    addWindow(p, 6 * 60, 7 * 60);
    addWindow(p, 17 * 60, 18 * 60);
    p.min_off_sec = DEFAULT_MIN_OFF_SEC;
    p.max_run_min = DEFAULT_MAX_RUN_MIN;
    p.recovery_delay_sec = DEFAULT_RECOVERY_DELAY_SEC;
    p.resume_after_outage = true;
    p.dry_run_current_a = DEFAULT_DRY_RUN_CURRENT_A;
    p.dry_run_grace_sec = DEFAULT_DRY_RUN_GRACE_SEC;
    p.tank_full_active_low = true;
    p.sump_dry_active_low = true;
}

static bool jsonString(JsonVariant v, char *dst, size_t n, bool keep_if_empty) {
    if (v.isNull()) return false;
    if (!v.is<const char *>()) return false;
    const char *s = v.as<const char *>();
    if (!s) return false;
    if (keep_if_empty && s[0] == '\0') return false;
    copyStr(dst, n, s);
    return true;
}

static bool parseWindowArray(JsonVariant v, SchedWindow *wins, uint8_t &nwin,
                             String &error, const char *label) {
    if (!v.is<JsonArray>()) {
        error = String(label) + ": windows must be an array";
        return false;
    }
    JsonArray arr = v.as<JsonArray>();
    if (arr.size() > MAX_WINDOWS) {
        error = String(label) + ": too many windows (max 4)";
        return false;
    }
    nwin = 0;
    for (JsonVariant item : arr) {
        if (!item.is<JsonObject>()) {
            error = String(label) + ": invalid window";
            return false;
        }
        JsonVariant on = item["on"];
        JsonVariant off = item["off"];
        if (!on.is<const char *>() || !off.is<const char *>()) {
            error = String(label) + ": window needs on/off strings";
            return false;
        }
        uint16_t o = hhmmToMinutes(on.as<const char *>());
        uint16_t f = hhmmToMinutes(off.as<const char *>());
        if (o == 0xFFFF || f == 0xFFFF) {
            error = String(label) + ": time must be HH:MM";
            return false;
        }
        wins[nwin].on_min = o;
        wins[nwin].off_min = f;
        nwin++;
    }
    return true;
}

static bool parseRecurring(JsonVariant v, Config &tmp, String &error) {
    if (!v.is<JsonArray>()) {
        error = "recurring must be an array";
        return false;
    }
    JsonArray arr = v.as<JsonArray>();
    if (arr.size() > MAX_RECURRING) {
        error = String("recurring: too many schedules (max ") + MAX_RECURRING + ")";
        return false;
    }
    tmp.nrecurring = 0;
    for (JsonVariant item : arr) {
        if (!item.is<JsonObject>()) {
            error = "recurring: each schedule must be an object";
            return false;
        }
        JsonObject o = item.as<JsonObject>();
        RecurringSchedule &rs = tmp.recurring[tmp.nrecurring];
        memset(&rs, 0, sizeof(rs));
        copyStr(rs.name, sizeof(rs.name), o["name"] | "Schedule");
        rs.enabled = o["enabled"] | true;

        const char *repeat = o["repeat"] | "daily";
        if (strcmp(repeat, "daily") == 0) rs.repeat = REPEAT_DAILY;
        else if (strcmp(repeat, "weekly") == 0) rs.repeat = REPEAT_WEEKLY;
        else if (strcmp(repeat, "monthly") == 0) rs.repeat = REPEAT_MONTHLY;
        else if (strcmp(repeat, "yearly") == 0) rs.repeat = REPEAT_YEARLY;
        else { error = "recurring: repeat must be daily/weekly/monthly/yearly"; return false; }

        int days = o["days"] | 127;
        if (days < 0 || days > 127) { error = "recurring: days must be 0..127"; return false; }
        rs.days = (uint8_t)days;

        int dom = o["day_of_month"] | 1;
        if (dom < 1 || dom > 31) { error = "recurring: day_of_month must be 1..31"; return false; }
        rs.day_of_month = (uint8_t)dom;

        int month = o["month"] | 1;
        if (month < 1 || month > 12) { error = "recurring: month must be 1..12"; return false; }
        rs.month = (uint8_t)month;

        const char *from = o["from"] | "06:00";
        const char *to = o["to"] | "07:00";
        uint16_t f = hhmmToMinutes(from);
        uint16_t t = hhmmToMinutes(to);
        if (f == 0xFFFF || t == 0xFFFF) { error = "recurring: from/to must be HH:MM"; return false; }
        rs.from_min = f;
        rs.to_min = t;

        uint16_t mask = 0;
        JsonVariant cm = o["channel_mask"];
        if (!cm.isNull()) {
            int m = cm.as<int>();
            if (m < 0 || m > 0x0FFF) { error = "recurring: channel_mask must be 0..4095"; return false; }
            mask = (uint16_t)m;
        }
        JsonArray chans = o["channels"].as<JsonArray>();
        if (chans) {
            for (JsonVariant c : chans) {
                int id = c.as<int>();
                if (id < 1 || id > MAX_CHANNELS) { error = "recurring: channel id must be 1..12"; return false; }
                mask |= (uint16_t)(1u << (id - 1));
            }
        }
        rs.channel_mask = mask;

        const char *action = o["action"] | "on";
        if (strcmp(action, "on") == 0) rs.action = RACT_ON;
        else if (strcmp(action, "off") == 0) rs.action = RACT_OFF;
        else if (strcmp(action, "auto") == 0) rs.action = RACT_AUTO;
        else { error = "recurring: action must be on/off/auto"; return false; }

        tmp.nrecurring++;
    }
    return true;
}

bool settingsFromJson(JsonDocument &doc, Config &cfg, String &error) {
    error = "";
    if (!doc.is<JsonObject>()) {
        error = "body must be a JSON object";
        return false;
    }

    Config tmp = cfg;
    JsonObject root = doc.as<JsonObject>();

    JsonObject dev = root["device"].as<JsonObject>();
    if (dev) {
        jsonString(dev["name"], tmp.device_name, sizeof(tmp.device_name), false);
        jsonString(dev["timezone"], tmp.timezone, sizeof(tmp.timezone), false);
        jsonString(dev["auth_user"], tmp.auth_user, sizeof(tmp.auth_user), false);
        jsonString(dev["auth_pass"], tmp.auth_pass, sizeof(tmp.auth_pass), true);
        jsonString(dev["device_id"], tmp.device_id, sizeof(tmp.device_id), true);
        const char *profile = dev["profile"];
        if (profile) {
            if (strcmp(profile, "agri") == 0) tmp.profile = PROFILE_AGRI;
            else if (strcmp(profile, "home") == 0) tmp.profile = PROFILE_HOME;
            else if (strcmp(profile, "godown") == 0) tmp.profile = PROFILE_GODOWN;
            else if (strcmp(profile, "industry") == 0) tmp.profile = PROFILE_INDUSTRY;
            else { error = "device: profile must be agri/home/godown/industry"; return false; }
        }
    }

    JsonObject wifi = root["wifi"].as<JsonObject>();
    if (wifi) {
        jsonString(wifi["ssid"], tmp.wifi_ssid, sizeof(tmp.wifi_ssid), false);
        jsonString(wifi["password"], tmp.wifi_pass, sizeof(tmp.wifi_pass), true);
        jsonString(wifi["hostname"], tmp.hostname, sizeof(tmp.hostname), false);
    }

    JsonObject coap = root["coap"].as<JsonObject>();
    if (coap) {
        if (!coap["enabled"].isNull()) tmp.coap_enabled = coap["enabled"].as<bool>();
        jsonString(coap["host"], tmp.coap_host, sizeof(tmp.coap_host), false);
        if (!coap["port"].isNull()) {
            int v = coap["port"].as<int>();
            if (v < 1 || v > 65535) { error = "coap: port must be 1..65535"; return false; }
            tmp.coap_port = (uint16_t)v;
        }
        if (!coap["path"].isNull()) {
            jsonString(coap["path"], tmp.coap_path, sizeof(tmp.coap_path), false);
            if (tmp.coap_path[0] != '/') {
                error = "coap: path must start with /";
                return false;
            }
        }
        if (!coap["interval_sec"].isNull()) {
            int v = coap["interval_sec"].as<int>();
            if (v < 10 || v > 3600) { error = "coap: interval_sec must be 10..3600"; return false; }
            tmp.coap_interval_sec = (uint16_t)v;
        }
        jsonString(coap["token"], tmp.coap_token, sizeof(tmp.coap_token), true);
        jsonString(coap["user"], tmp.remote_user, sizeof(tmp.remote_user), false);
        jsonString(coap["pass"], tmp.remote_pass, sizeof(tmp.remote_pass), true);
        if (!coap["register"].isNull()) tmp.remote_register = coap["register"].as<bool>();
        if (tmp.coap_enabled && tmp.coap_host[0] == '\0') {
            error = "coap: host required when enabled";
            return false;
        }
    }

    JsonArray chans = root["channels"].as<JsonArray>();
    if (chans) {
        uint8_t i = 0;
        for (JsonVariant v : chans) {
            if (i >= MAX_CHANNELS) break;
            if (!v.is<JsonObject>()) {
                error = "channels[] entries must be objects";
                return false;
            }
            JsonObject o = v.as<JsonObject>();
            ChannelConfig &ch = tmp.channels[i];
            jsonString(o["name"], ch.name, sizeof(ch.name), false);
            jsonString(o["zone"], ch.zone, sizeof(ch.zone), false);
            const char *eq = o["equipment"];
            if (eq) {
                if (strcmp(eq, "valve") == 0) ch.equipment = EQ_VALVE;
                else if (strcmp(eq, "light") == 0) ch.equipment = EQ_LIGHT;
                else if (strcmp(eq, "motor") == 0) ch.equipment = EQ_MOTOR;
                else if (strcmp(eq, "fan") == 0) ch.equipment = EQ_FAN;
                else if (strcmp(eq, "pump") == 0) ch.equipment = EQ_PUMP;
                else if (strcmp(eq, "socket") == 0) ch.equipment = EQ_SOCKET;
                else if (strcmp(eq, "heater") == 0) ch.equipment = EQ_HEATER;
                else if (strcmp(eq, "door") == 0) ch.equipment = EQ_DOOR;
                else if (strcmp(eq, "other") == 0) ch.equipment = EQ_OTHER;
                else { error = String("channel ") + (i + 1) + ": unknown equipment"; return false; }
            }
            if (!o["pulse_mode"].isNull()) ch.pulse_mode = o["pulse_mode"].as<bool>();
            if (!o["pulse_on_ms"].isNull()) {
                int v2 = o["pulse_on_ms"].as<int>();
                if (v2 < 20 || v2 > 10000) { error = String("channel ") + (i + 1) + ": pulse_on_ms must be 20..10000"; return false; }
                ch.pulse_on_ms = (uint16_t)v2;
            }
            if (!o["pulse_off_ms"].isNull()) {
                int v2 = o["pulse_off_ms"].as<int>();
                if (v2 < 20 || v2 > 10000) { error = String("channel ") + (i + 1) + ": pulse_off_ms must be 20..10000"; return false; }
                ch.pulse_off_ms = (uint16_t)v2;
            }
            if (!o["enabled"].isNull()) ch.enabled = o["enabled"].as<bool>();
            if (!o["days"].isNull()) {
                int d = o["days"].as<int>();
                if (d < 0 || d > 127) {
                    error = String("channel ") + (i + 1) + ": days must be 0..127";
                    return false;
                }
                ch.days = (uint8_t)d;
            }
            if (!o["mode"].isNull()) {
                bool ok = false;
                RunMode m = modeFromString(o["mode"].as<const char *>(), ok);
                if (!ok) {
                    error = String("channel ") + (i + 1) + ": mode must be auto/on/off";
                    return false;
                }
                ch.mode = m;
            }
            if (!o["windows"].isNull()) {
                String label = String("channel ") + (i + 1);
                if (!parseWindowArray(o["windows"], ch.win, ch.nwin, error, label.c_str()))
                    return false;
            }
            i++;
        }
    }

    JsonVariant rec = root["recurring"];
    if (!rec.isNull()) {
        if (!parseRecurring(rec, tmp, error)) return false;
    }

    JsonObject p = root["pump"].as<JsonObject>();
    if (p) {
        PumpConfig &pc = tmp.pump;
        jsonString(p["name"], pc.name, sizeof(pc.name), false);
        if (!p["enabled"].isNull()) pc.enabled = p["enabled"].as<bool>();
        if (!p["days"].isNull()) {
            int d = p["days"].as<int>();
            if (d < 0 || d > 127) { error = "pump: days must be 0..127"; return false; }
            pc.days = (uint8_t)d;
        }
        if (!p["mode"].isNull()) {
            bool ok = false;
            RunMode m = modeFromString(p["mode"].as<const char *>(), ok);
            if (!ok) { error = "pump: mode must be auto/on/off"; return false; }
            pc.mode = m;
        }
        if (!p["windows"].isNull()) {
            if (!parseWindowArray(p["windows"], pc.win, pc.nwin, error, "pump"))
                return false;
        }
        if (!p["min_off_sec"].isNull()) {
            int v = p["min_off_sec"].as<int>();
            if (v < 30 || v > 3600) { error = "pump: min_off_sec must be 30..3600"; return false; }
            pc.min_off_sec = (uint16_t)v;
        }
        if (!p["max_run_min"].isNull()) {
            int v = p["max_run_min"].as<int>();
            if (v < 1 || v > 720) { error = "pump: max_run_min must be 1..720"; return false; }
            pc.max_run_min = (uint16_t)v;
        }
        if (!p["recovery_delay_sec"].isNull()) {
            int v = p["recovery_delay_sec"].as<int>();
            if (v < 0 || v > 600) { error = "pump: recovery_delay_sec must be 0..600"; return false; }
            pc.recovery_delay_sec = (uint16_t)v;
        }
        if (!p["resume_after_outage"].isNull())
            pc.resume_after_outage = p["resume_after_outage"].as<bool>();
        if (!p["dry_run_current_a"].isNull()) {
            float v = p["dry_run_current_a"].as<float>();
            if (v < 0.5f || v > 20.0f) { error = "pump: dry_run_current_a must be 0.5..20"; return false; }
            pc.dry_run_current_a = v;
        }
        if (!p["dry_run_grace_sec"].isNull()) {
            int v = p["dry_run_grace_sec"].as<int>();
            if (v < 5 || v > 120) { error = "pump: dry_run_grace_sec must be 5..120"; return false; }
            pc.dry_run_grace_sec = (uint16_t)v;
        }
        if (!p["tank_full_active_low"].isNull())
            pc.tank_full_active_low = p["tank_full_active_low"].as<bool>();
        if (!p["sump_dry_active_low"].isNull())
            pc.sump_dry_active_low = p["sump_dry_active_low"].as<bool>();
    }

    JsonObject safety = root["safety"].as<JsonObject>();
    if (safety && !safety["max_boost_min"].isNull()) {
        int v = safety["max_boost_min"].as<int>();
        if (v < 1 || v > 1440) { error = "safety: max_boost_min must be 1..1440"; return false; }
        tmp.max_boost_min = (uint16_t)v;
    }

    cfg = tmp;
    return true;
}

void settingsToJson(const Config &cfg, JsonDocument &doc, bool include_secrets) {
    JsonObject root = doc.to<JsonObject>();

    JsonObject dev = root["device"].to<JsonObject>();
    dev["name"] = cfg.device_name;
    dev["timezone"] = cfg.timezone;
    dev["auth_user"] = cfg.auth_user;
    dev["device_id"] = cfg.device_id;
    dev["profile"] = profileName(cfg.profile);
    if (include_secrets) dev["auth_pass"] = cfg.auth_pass;

    JsonObject wifi = root["wifi"].to<JsonObject>();
    wifi["ssid"] = cfg.wifi_ssid;
    wifi["hostname"] = cfg.hostname;
    if (include_secrets) wifi["password"] = cfg.wifi_pass;

    JsonObject coap = root["coap"].to<JsonObject>();
    coap["enabled"] = cfg.coap_enabled;
    coap["host"] = cfg.coap_host;
    coap["port"] = cfg.coap_port;
    coap["path"] = cfg.coap_path;
    coap["interval_sec"] = cfg.coap_interval_sec;
    coap["user"] = cfg.remote_user;
    coap["register"] = cfg.remote_register;
    if (include_secrets) {
        coap["token"] = cfg.coap_token;
        coap["pass"] = cfg.remote_pass;
    }

    JsonArray chans = root["channels"].to<JsonArray>();
    for (uint8_t i = 0; i < MAX_CHANNELS; i++) {
        const ChannelConfig &ch = cfg.channels[i];
        JsonObject o = chans.add<JsonObject>();
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
        JsonArray wins = o["windows"].to<JsonArray>();
        for (uint8_t w = 0; w < ch.nwin; w++) {
            JsonObject wj = wins.add<JsonObject>();
            char on[6], off[6];
            minutesToHHMM(ch.win[w].on_min, on, sizeof(on));
            minutesToHHMM(ch.win[w].off_min, off, sizeof(off));
            wj["on"] = on;
            wj["off"] = off;
        }
    }

    JsonArray rec = root["recurring"].to<JsonArray>();
    for (uint8_t i = 0; i < cfg.nrecurring; i++) {
        const RecurringSchedule &rs = cfg.recurring[i];
        JsonObject o = rec.add<JsonObject>();
        o["name"] = rs.name;
        o["enabled"] = rs.enabled;
        o["repeat"] = repeatName(rs.repeat);
        o["days"] = rs.days;
        o["day_of_month"] = rs.day_of_month;
        o["month"] = rs.month;
        char from[6], to[6];
        minutesToHHMM(rs.from_min, from, sizeof(from));
        minutesToHHMM(rs.to_min, to, sizeof(to));
        o["from"] = from;
        o["to"] = to;
        o["channel_mask"] = rs.channel_mask;
        JsonArray chans = o["channels"].to<JsonArray>();
        for (uint8_t c = 0; c < MAX_CHANNELS; c++) {
            if (rs.channel_mask & (uint16_t)(1u << c)) chans.add(c + 1);
        }
        o["action"] = actionName(rs.action);
    }

    const PumpConfig &p = cfg.pump;
    JsonObject pj = root["pump"].to<JsonObject>();
    pj["name"] = p.name;
    pj["enabled"] = p.enabled;
    pj["days"] = p.days;
    pj["mode"] = modeName(p.mode);
    JsonArray pw = pj["windows"].to<JsonArray>();
    for (uint8_t w = 0; w < p.nwin; w++) {
        JsonObject wj = pw.add<JsonObject>();
        char on[6], off[6];
        minutesToHHMM(p.win[w].on_min, on, sizeof(on));
        minutesToHHMM(p.win[w].off_min, off, sizeof(off));
        wj["on"] = on;
        wj["off"] = off;
    }
    pj["min_off_sec"] = p.min_off_sec;
    pj["max_run_min"] = p.max_run_min;
    pj["recovery_delay_sec"] = p.recovery_delay_sec;
    pj["resume_after_outage"] = p.resume_after_outage;
    pj["dry_run_current_a"] = p.dry_run_current_a;
    pj["dry_run_grace_sec"] = p.dry_run_grace_sec;
    pj["tank_full_active_low"] = p.tank_full_active_low;
    pj["sump_dry_active_low"] = p.sump_dry_active_low;

    JsonObject safety = root["safety"].to<JsonObject>();
    safety["max_boost_min"] = cfg.max_boost_min;
}

bool settingsSave(const Config &cfg) {
    JsonDocument doc;
    settingsToJson(cfg, doc, true);
    String s;
    serializeJson(doc, s);
    Preferences prefs;
    if (!prefs.begin("mraf", false)) return false;
    bool ok = prefs.putString("cfg", s) > 0;
    prefs.end();
    return ok;
}

bool settingsLoad(Config &cfg) {
    Preferences prefs;
    if (!prefs.begin("mraf", true)) return false;
    String s = prefs.getString("cfg", "");
    prefs.end();
    if (s.length() < 2) return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, s);
    if (err) return false;

    String error;
    Config tmp = cfg;
    if (!settingsFromJson(doc, tmp, error)) return false;
    cfg = tmp;
    return true;
}

uint16_t hhmmToMinutes(const char *s) {
    if (!s) return 0xFFFF;
    int h = -1, m = -1;
    char tail = 0;
    if (sscanf(s, "%d:%d%c", &h, &m, &tail) != 2) return 0xFFFF;
    if (h < 0 || h > 23 || m < 0 || m > 59) return 0xFFFF;
    return (uint16_t)(h * 60 + m);
}

void minutesToHHMM(uint16_t m, char *out, size_t n) {
    if (!out || n < 6) return;
    snprintf(out, n, "%02u:%02u", (unsigned)(m / 60), (unsigned)(m % 60));
}

const char *modeName(RunMode m) {
    switch (m) {
        case MODE_ON: return "on";
        case MODE_OFF: return "off";
        default: return "auto";
    }
}

RunMode modeFromString(const char *s, bool &ok) {
    ok = true;
    if (!s) { ok = false; return MODE_AUTO; }
    if (strcmp(s, "auto") == 0) return MODE_AUTO;
    if (strcmp(s, "on") == 0) return MODE_ON;
    if (strcmp(s, "off") == 0) return MODE_OFF;
    ok = false;
    return MODE_AUTO;
}

const char *profileName(uint8_t p) {
    switch (p) {
        case PROFILE_HOME: return "home";
        case PROFILE_GODOWN: return "godown";
        case PROFILE_INDUSTRY: return "industry";
        default: return "agri";
    }
}

const char *equipmentName(uint8_t e) {
    switch (e) {
        case EQ_VALVE: return "valve";
        case EQ_LIGHT: return "light";
        case EQ_MOTOR: return "motor";
        case EQ_FAN: return "fan";
        case EQ_PUMP: return "pump";
        case EQ_SOCKET: return "socket";
        case EQ_HEATER: return "heater";
        case EQ_DOOR: return "door";
        default: return "other";
    }
}

const char *repeatName(uint8_t r) {
    switch (r) {
        case REPEAT_WEEKLY: return "weekly";
        case REPEAT_MONTHLY: return "monthly";
        case REPEAT_YEARLY: return "yearly";
        default: return "daily";
    }
}

const char *actionName(uint8_t a) {
    switch (a) {
        case RACT_OFF: return "off";
        case RACT_AUTO: return "auto";
        default: return "on";
    }
}

String localTimeString(time_t t) {
    struct tm tm;
    localtime_r(&t, &tm);
    char buf[24];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    return String(buf);
}
