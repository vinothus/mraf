// native_sim platform implementation.
//
// All hardware is emulated in memory so the full application (HTTP server,
// controller, persistence, remote poller) can run on a development host.
#include "mraf_platform.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>

#include "config.h"
#include "mraf_compat.h"
#include "mraf_hal.h"
#include "mraf_storage.h"

#ifndef CONFIG_MRAF_SETTINGS_PATH
#define CONFIG_MRAF_SETTINGS_PATH "mraf-settings.json"
#endif

static int g_pin_state[64];
static char g_timezone[48] = "UTC0";

static bool g_sim_ready;
static bool g_sim_power_state = true;
static bool g_sim_tank;
static bool g_sim_sump;
static bool g_sim_current_auto = true;
static float g_sim_current_a;
static float g_sim_battery_v = 13.2f;

static bool g_rtc_ok;
static time_t g_rtc_epoch;

static void sim_init_once(void)
{
    if (g_sim_ready) {
        return;
    }
    g_sim_ready = true;
    for (int i = 0; i < 64; i++) {
        g_pin_state[i] = -1;
    }
}

void mraf_sim_init(void)
{
    sim_init_once();
}

void mraf_platform_init(void)
{
    sim_init_once();
    mraf_set_timezone("UTC0");
}

const char *mraf_platform_name(void)
{
    return "native_sim";
}

bool mraf_platform_is_simulator(void)
{
    return true;
}

bool mraf_platform_is_ap_mode(void)
{
    return false;
}

void mraf_platform_start_network(const Config &cfg)
{
    (void)cfg;
}

void mraf_platform_poll_network(void)
{
}

void mraf_platform_net_info(bool *connected, char *ip, size_t ip_cap,
                            char *ssid, size_t ssid_cap, int *rssi)
{
    if (connected) {
        *connected = true;
    }
    if (ip && ip_cap) {
        snprintf(ip, ip_cap, "127.0.0.1");
    }
    if (ssid && ssid_cap) {
        snprintf(ssid, ssid_cap, "native-sim");
    }
    if (rssi) {
        *rssi = -42;
    }
}

void mraf_platform_wifi_scan(JsonDocument &doc)
{
    doc["networks"].to<JsonArray>();
}

bool mraf_platform_time_sync(void)
{
    return true;
}

void mraf_platform_set_epoch(time_t t)
{
    g_rtc_epoch = t;
    g_rtc_ok = true;
}

bool mraf_platform_rtc_begin(void)
{
    return true;
}

bool mraf_platform_rtc_read(time_t *out)
{
    if (!g_rtc_ok) {
        return false;
    }
    if (out) {
        *out = g_rtc_epoch;
    }
    return true;
}

void mraf_platform_rtc_write(time_t t)
{
    g_rtc_epoch = t;
    g_rtc_ok = true;
}

void mraf_platform_led(bool on)
{
    (void)on;
}

void mraf_platform_restart(void)
{
    sys_reboot(SYS_REBOOT_COLD);
}

bool mraf_platform_mains_present(void)
{
    return g_sim_power_state;
}

bool mraf_platform_read_tank_full(bool active_low)
{
    return active_low ? g_sim_tank : !g_sim_tank;
}

bool mraf_platform_read_sump_dry(bool active_low)
{
    return active_low ? g_sim_sump : !g_sim_sump;
}

float mraf_platform_read_current_a(bool pump_commanded_on)
{
    if (!g_sim_current_auto) {
        return g_sim_current_a;
    }
    if (pump_commanded_on && g_sim_power_state && !g_sim_tank && !g_sim_sump) {
        return 6.8f;
    }
    return 0.0f;
}

float mraf_platform_read_battery_v(void)
{
    return g_sim_battery_v;
}

void mraf_sim_set_power(bool present)
{
    g_sim_power_state = present;
}

void mraf_sim_set_sensors(bool tank_full, bool sump_dry)
{
    g_sim_tank = tank_full;
    g_sim_sump = sump_dry;
}

void mraf_sim_set_current(float amps)
{
    g_sim_current_auto = false;
    g_sim_current_a = amps < 0.0f ? 0.0f : amps;
}

void mraf_sim_set_battery(float volts)
{
    g_sim_battery_v = volts;
}

bool mraf_sim_power(void) { return g_sim_power_state; }
bool mraf_sim_tank_full(void) { return g_sim_tank; }
bool mraf_sim_sump_dry(void) { return g_sim_sump; }
float mraf_sim_current(void) { return g_sim_current_a; }
float mraf_sim_battery(void) { return g_sim_battery_v; }
bool mraf_sim_current_is_auto(void) { return g_sim_current_auto; }

void mraf_sim_set_current_auto(void)
{
    g_sim_current_auto = true;
    g_sim_current_a = 0.0f;
}

// --- GPIO / storage -------------------------------------------------------

void mraf_gpio_config(uint8_t pin, uint8_t mode)
{
    (void)mode;
    sim_init_once();
    if (pin < 64 && g_pin_state[pin] < 0) {
        g_pin_state[pin] = HIGH;
    }
}

void mraf_gpio_write(uint8_t pin, uint8_t level)
{
    sim_init_once();
    if (pin < 64) {
        g_pin_state[pin] = level ? HIGH : LOW;
    }
}

int mraf_gpio_read(uint8_t pin)
{
    sim_init_once();
    if (pin == MAINS_SENSE_PIN) {
        return mraf_platform_mains_present() == MAINS_SENSE_ACTIVE_HIGH ? HIGH : LOW;
    }
    if (pin == TANK_FULL_PIN) {
        return g_sim_tank ? LOW : HIGH;
    }
    if (pin == SUMP_DRY_PIN) {
        return g_sim_sump ? LOW : HIGH;
    }
    if (pin < 64 && g_pin_state[pin] >= 0) {
        return g_pin_state[pin];
    }
    return LOW;
}

unsigned long mraf_gpio_millis(void)
{
    return k_uptime_get_32();
}

// --- persistence ----------------------------------------------------------

bool mraf_storage_load(char *buf, size_t cap)
{
    if (!buf || cap < 2) {
        return false;
    }
    FILE *f = fopen(CONFIG_MRAF_SETTINGS_PATH, "rb");
    if (!f) {
        return false;
    }
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return n > 0;
}

bool mraf_storage_save(const char *json)
{
    if (!json) {
        return false;
    }
    FILE *f = fopen(CONFIG_MRAF_SETTINGS_PATH, "wb");
    if (!f) {
        return false;
    }
    size_t len = strlen(json);
    size_t n = fwrite(json, 1, len, f);
    fclose(f);
    return n == len;
}

void mraf_storage_init(void)
{
}

// --- POSIX TZ aware localtime ---------------------------------------------

void mraf_set_timezone(const char *tz)
{
    if (!tz) {
        tz = "UTC0";
    }
    snprintf(g_timezone, sizeof(g_timezone), "%s", tz);
}

static int32_t tz_offset_seconds(void)
{
    // POSIX TZ: std offset, where offset is the time to ADD to local time to
    // obtain UTC. "IST-5:30" therefore means UTC+05:30.
    const char *p = g_timezone;
    while (*p && !(*p == '+' || *p == '-' ||
                    (*p >= '0' && *p <= '9'))) {
        p++;
    }
    if (!*p) {
        return 0;
    }
    int sign = 1;
    if (*p == '+' || *p == '-') {
        sign = (*p == '-') ? -1 : 1;
        p++;
    }
    int hours = 0, minutes = 0, seconds = 0;
    while (*p >= '0' && *p <= '9') {
        hours = hours * 10 + (*p - '0');
        p++;
    }
    if (*p == ':') {
        p++;
        while (*p >= '0' && *p <= '9') {
            minutes = minutes * 10 + (*p - '0');
            p++;
        }
    }
    if (*p == ':') {
        p++;
        while (*p >= '0' && *p <= '9') {
            seconds = seconds * 10 + (*p - '0');
            p++;
        }
    }
    int32_t local_to_utc = sign * (hours * 3600 + minutes * 60 + seconds);
    // Local time is ahead of UTC by -local_to_utc.
    return -local_to_utc;
}

struct tm *mraf_localtime_r(const time_t *t, struct tm *out)
{
    if (!t || !out) {
        return nullptr;
    }
    time_t local = *t + tz_offset_seconds();
    return gmtime_r(&local, out);
}
