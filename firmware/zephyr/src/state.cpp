// Shared state, virtual simulator clock and dashboard status serialization.
#include "mraf_state.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <zephyr/kernel.h>

#include "config.h"
#include "mraf_compat.h"
#include "mraf_platform.h"
#include "remote.h"
#include "settings.h"

Config g_cfg;
Controller g_ctrl;
struct k_mutex g_state_lock;

static struct k_spinlock g_clock_lock;
static int64_t g_base_epoch_us;
static int64_t g_base_uptime_ms;
static int64_t g_offset_us;
static int32_t g_speed_x1000 = 1000;
static bool g_time_valid;
static time_t g_last_control_tick;

void mraf_state_init(void)
{
    k_mutex_init(&g_state_lock);
    settingsDefaults(g_cfg);
    (void)settingsLoad(g_cfg);
    mraf_set_timezone(g_cfg.timezone);
    mraf_sim_init();
    mraf_sim_set_defaults();
    g_ctrl.begin(g_cfg);
    g_last_control_tick = mraf_now();
}

void mraf_tick_catchup(void)
{
    time_t now = mraf_now();
    if (g_last_control_tick == 0) {
        g_last_control_tick = now;
        return;
    }
    if (now <= g_last_control_tick) {
        if (now < g_last_control_tick) {
            g_last_control_tick = now;
        }
        return;
    }

    k_mutex_lock(&g_state_lock, K_FOREVER);
    bool power = mraf_platform_mains_present();
    bool tank = mraf_platform_read_tank_full(g_cfg.pump.tank_full_active_low);
    bool sump = mraf_platform_read_sump_dry(g_cfg.pump.sump_dry_active_low);
    bool pump_on = g_ctrl.pump().on;
    float current = mraf_platform_read_current_a(pump_on);
    float battery = mraf_platform_read_battery_v();

    int64_t steps = (int64_t)now - (int64_t)g_last_control_tick;
    if (steps > 200000) {
        g_last_control_tick = now - 200000;
    }
    for (time_t t = g_last_control_tick + 1; t <= now; t++) {
        g_ctrl.tick(t, power, tank, sump, current, battery);
    }
    g_last_control_tick = now;
    k_mutex_unlock(&g_state_lock);
}

void mraf_apply_inputs_locked(time_t now)
{
    bool power = mraf_platform_mains_present();
    bool tank = mraf_platform_read_tank_full(g_cfg.pump.tank_full_active_low);
    bool sump = mraf_platform_read_sump_dry(g_cfg.pump.sump_dry_active_low);
    bool pump_on = g_ctrl.pump().on;
    float current = mraf_platform_read_current_a(pump_on);
    float battery = mraf_platform_read_battery_v();
    g_ctrl.tick(now, power, tank, sump, current, battery);
    g_last_control_tick = now;
}

void mraf_tick_reset(void)
{
    g_last_control_tick = mraf_now();
}

void mraf_service_pulses(void)
{
    k_mutex_lock(&g_state_lock, K_FOREVER);
    g_ctrl.setPulseClock(k_uptime_get_32());
    g_ctrl.servicePulses();
    k_mutex_unlock(&g_state_lock);
}

void mraf_sim_set_defaults(void)
{
    mraf_sim_set_power(true);
    mraf_sim_set_sensors(false, false);
    mraf_sim_set_current_auto();
    mraf_sim_set_battery(13.2f);
}

static int64_t clock_now_us_locked(void)
{
    int64_t elapsed_ms = k_uptime_get() - g_base_uptime_ms;
    if (elapsed_ms < 0) {
        elapsed_ms = 0;
    }
    return g_base_epoch_us + g_offset_us + elapsed_ms * g_speed_x1000;
}

void mraf_clock_init(time_t base)
{
    k_spinlock_key_t key = k_spin_lock(&g_clock_lock);
    g_base_epoch_us = (int64_t)base * 1000000LL;
    g_base_uptime_ms = k_uptime_get();
    g_offset_us = 0;
    g_time_valid = base > 1700000000;
    k_spin_unlock(&g_clock_lock, key);
}

time_t mraf_now(void)
{
    k_spinlock_key_t key = k_spin_lock(&g_clock_lock);
    int64_t us = clock_now_us_locked();
    g_base_uptime_ms = k_uptime_get();
    g_base_epoch_us = us;
    g_offset_us = 0;
    k_spin_unlock(&g_clock_lock, key);
    return (time_t)(us / 1000000LL);
}

void mraf_set_now(time_t t)
{
    k_spinlock_key_t key = k_spin_lock(&g_clock_lock);
    g_base_epoch_us = (int64_t)t * 1000000LL;
    g_base_uptime_ms = k_uptime_get();
    g_offset_us = 0;
    g_time_valid = true;
    k_spin_unlock(&g_clock_lock, key);
}

void mraf_advance_time(int64_t seconds)
{
    k_spinlock_key_t key = k_spin_lock(&g_clock_lock);
    g_offset_us += seconds * 1000000LL;
    k_spin_unlock(&g_clock_lock, key);
}

int32_t mraf_time_speed(void)
{
    return g_speed_x1000;
}

void mraf_set_time_speed(int32_t speed_x1000)
{
    if (speed_x1000 < 10) {
        speed_x1000 = 10;
    }
    k_spinlock_key_t key = k_spin_lock(&g_clock_lock);
    int64_t now_us = clock_now_us_locked();
    g_base_epoch_us = now_us;
    g_base_uptime_ms = k_uptime_get();
    g_offset_us = 0;
    g_speed_x1000 = speed_x1000;
    k_spin_unlock(&g_clock_lock, key);
}

bool mraf_time_valid(void)
{
    return g_time_valid;
}

static void addWindows(JsonObject obj, const SchedWindow *win, uint8_t n)
{
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

void mraf_build_status(JsonDocument &doc)
{
    time_t now = mraf_now();
    bool connected = false;
    char ip[40] = "";
    char ssid[40] = "";
    int rssi = 0;
    mraf_platform_net_info(&connected, ip, sizeof(ip), ssid, sizeof(ssid), &rssi);
    bool ap = mraf_platform_is_ap_mode();

    doc["simulator"] = mraf_platform_is_simulator();
    doc["fw"] = FW_VERSION;
    doc["device"] = g_cfg.device_name;
    doc["now"] = (uint32_t)now;
    doc["now_local"] = localTimeString(now);
    doc["time_valid"] = mraf_time_valid();
    doc["uptime_sec"] = (uint32_t)(k_uptime_get() / 1000);

    JsonObject w = doc["wifi"].to<JsonObject>();
    w["mode"] = ap ? "ap" : "sta";
    w["ssid"] = ap ? g_cfg.device_name : ssid;
    w["ip"] = ip;
    w["rssi"] = ap ? 0 : rssi;
    w["hostname"] = g_cfg.hostname;
    w["connected"] = connected;

    JsonObject rem = doc["remote"].to<JsonObject>();
    mraf_remote_build_status(rem);

    doc["profile"] = profileName(g_cfg.profile);

    JsonObject pw = doc["power"].to<JsonObject>();
    pw["present"] = g_ctrl.powerPresent();
    pw["battery_v"] = g_ctrl.batteryVoltage();
    pw["outage_sec"] = (uint32_t)g_ctrl.outageSeconds(now);
    pw["battery_backed"] = true;

    JsonObject s = doc["sensors"].to<JsonObject>();
    s["tank_full"] = g_ctrl.pump().tank_full;
    s["sump_dry"] = g_ctrl.pump().sump_dry;
    s["acs_zero_mv"] = 1250.0f;

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

    if (mraf_platform_is_simulator()) {
        JsonObject sim = doc["sim"].to<JsonObject>();
        struct tm tm;
        localtime_r(&now, &tm);
        static const char *wd_names[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
        sim["mains"] = mraf_sim_power();
        sim["tank_full"] = mraf_sim_tank_full();
        sim["sump_dry"] = mraf_sim_sump_dry();
        if (mraf_sim_current_is_auto()) {
            sim["current_override"] = nullptr;
        } else {
            sim["current_override"] = mraf_sim_current();
        }
        sim["battery_v"] = mraf_sim_battery();
        sim["speed"] = (float)mraf_time_speed() / 1000.0f;
        sim["now_epoch"] = (uint32_t)now;
        sim["timezone"] = g_cfg.timezone;
        sim["weekday"] = wd_names[(tm.tm_wday + 6) % 7];
    }
}

void mraf_build_remote_data(JsonObject data, time_t now)
{
    bool connected = false;
    char ip[40] = "";
    char ssid[40] = "";
    int rssi = 0;
    mraf_platform_net_info(&connected, ip, sizeof(ip), ssid, sizeof(ssid), &rssi);
    bool ap = mraf_platform_is_ap_mode();

    data["now"] = (uint32_t)now;
    data["power"] = g_ctrl.powerPresent();
    data["battery_v"] = g_ctrl.batteryVoltage();
    data["tank_full"] = g_ctrl.pump().tank_full;
    data["sump_dry"] = g_ctrl.pump().sump_dry;
    data["wifi_rssi"] = ap ? 0 : rssi;
    data["ip"] = ip;
    data["profile"] = profileName(g_cfg.profile);
    data["confirmed"] = mraf_remote_confirmed();

    JsonObject p = data["pump"].to<JsonObject>();
    p["on"] = g_ctrl.pump().on;
    p["state"] = g_ctrl.pumpStateName(now);
    p["mode"] = modeName(g_ctrl.config().pump.mode);
    p["current_a"] = g_ctrl.pump().current_a;
    p["fault"] = g_ctrl.pump().fault;
    p["fault_latched"] = g_ctrl.pump().fault_latched;

    JsonArray relays = data["relays"].to<JsonArray>();
    for (uint8_t i = 0; i < MAX_CHANNELS; i++) {
        relays.add(g_ctrl.relay(i).on ? 1 : 0);
    }

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

bool mraf_apply_control(JsonObject cmd, time_t now, String &error)
{
    const char *target = cmd["target"] | "";
    const char *modeStr = cmd["mode"] | "auto";
    bool ok = false;
    RunMode mode = modeFromString(modeStr, ok);
    if (!ok) {
        error = "mode must be auto/on/off";
        return false;
    }
    int boost_i = cmd["boost_min"] | 0;
    if (boost_i < 0) {
        boost_i = 0;
    }
    uint16_t boost = (uint16_t)boost_i;
    int pulse_i = cmd["pulse_ms"] | 0;
    if (pulse_i < 0) {
        pulse_i = 0;
    }
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
        if (mode == MODE_OFF) {
            g_ctrl.allOff(now);
        } else if (mode == MODE_AUTO) {
            g_ctrl.allAuto(now);
        } else {
            error = "all supports auto/off only";
            return false;
        }
    } else {
        error = "target must be relay/pump/all";
        return false;
    }
    return true;
}
