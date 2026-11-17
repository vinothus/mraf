// native_sim platform implementation.
//
// All hardware is emulated in memory so the full application (HTTP server,
// controller, persistence, remote poller) can run on a development host.
#include "mraf_platform.h"
#include "webrtc_peer.h"

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
#include "model.h"

#if defined(CONFIG_NET_DHCPV4)
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>

static struct net_mgmt_event_callback g_dhcp_cb;
static struct k_sem g_dhcp_sem;
static char g_net_ip[NET_IPV4_ADDR_LEN];
#endif

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

#if defined(CONFIG_NET_DHCPV4)
static void dhcp_event_handler(struct net_mgmt_event_callback *cb,
                               uint64_t event, struct net_if *iface)
{
    if (event != NET_EVENT_IPV4_ADDR_ADD || !iface) {
        return;
    }
    struct net_in_addr *addr =
        net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
    if (addr) {
        net_addr_ntop(AF_INET, addr, g_net_ip, sizeof(g_net_ip));
        k_sem_give(&g_dhcp_sem);
    }
}
#endif

void mraf_platform_init(void)
{
    sim_init_once();
    mraf_set_timezone("UTC0");
#if defined(CONFIG_NET_DHCPV4)
    k_sem_init(&g_dhcp_sem, 0, 1);
#endif
}

const char *mraf_platform_name(void)
{
#if defined(CONFIG_BOARD_QEMU_X86)
    return "qemu_x86";
#else
    return "native_sim";
#endif
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
#if defined(CONFIG_NET_DHCPV4)
    struct net_if *iface = net_if_get_default();
    if (!iface) {
        return;
    }
    net_mgmt_init_event_callback(&g_dhcp_cb, dhcp_event_handler,
                                 NET_EVENT_IPV4_ADDR_ADD);
    net_mgmt_add_event_callback(&g_dhcp_cb);
    k_sem_reset(&g_dhcp_sem);
    net_dhcpv4_start(iface);
    // Slirp (QEMU user networking) answers in well under a second; the
    // timeout only covers a backend with no DHCP server at all.
    (void)k_sem_take(&g_dhcp_sem, K_SECONDS(20));
#endif
}

void mraf_platform_poll_network(void)
{
}

void mraf_platform_net_info(bool *connected, char *ip, size_t ip_cap,
                            char *ssid, size_t ssid_cap, int *rssi)
{
#if defined(CONFIG_NET_DHCPV4)
    bool have_ip = g_net_ip[0] != '\0';
    const char *current_ip = have_ip ? g_net_ip : "127.0.0.1";
#else
    bool have_ip = false;
    const char *current_ip = "127.0.0.1";
#endif
    if (connected) {
        *connected = true;
    }
    if (ip && ip_cap) {
        snprintf(ip, ip_cap, "%s", current_ip);
    }
    if (ssid && ssid_cap) {
        snprintf(ssid, ssid_cap, "%s", have_ip ? "qemu-net" : "native-sim");
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

// The native simulator records the tunnel configuration but has no overlay to
// install it into.
static bool g_wg_active;

bool mraf_platform_wireguard_apply(const WireGuardConfig &cfg)
{
    g_wg_active = cfg.enabled && cfg.address[0] != '\0';
    printk("WireGuard(native_sim): config %s (address %s)\n",
           g_wg_active ? "accepted" : "ignored", cfg.address);
    return g_wg_active;
}

bool mraf_platform_wireguard_active(void)
{
    return g_wg_active;
}

// The portable DTLS/SCTP peer (webrtc_peer.cpp) provides the transport on
// both native_sim and ESP32; it is only linked when CONFIG_MRAF_WEBRTC is on.
bool mraf_platform_webrtc_available(void)
{
#if defined(CONFIG_MRAF_WEBRTC)
    return mraf_peer_available();
#else
    return false;
#endif
}

bool mraf_platform_webrtc_start(const char *offer_sdp, char *answer_sdp,
                                size_t answer_cap)
{
#if defined(CONFIG_MRAF_WEBRTC)
    return mraf_peer_start(offer_sdp, answer_sdp, answer_cap);
#else
    ARG_UNUSED(offer_sdp);
    if (answer_sdp && answer_cap) {
        answer_sdp[0] = '\0';
    }
    return false;
#endif
}

void mraf_platform_webrtc_stop(void)
{
#if defined(CONFIG_MRAF_WEBRTC)
    mraf_peer_stop();
#endif
}

bool mraf_platform_webrtc_open(void)
{
#if defined(CONFIG_MRAF_WEBRTC)
    return mraf_peer_open();
#else
    return false;
#endif
}

int mraf_platform_webrtc_send(const char *message)
{
#if defined(CONFIG_MRAF_WEBRTC)
    return mraf_peer_send(message);
#else
    ARG_UNUSED(message);
    return -1;
#endif
}

char *mraf_platform_webrtc_tx_begin(size_t *cap)
{
#if defined(CONFIG_MRAF_WEBRTC)
    return mraf_peer_tx_begin(cap);
#else
    ARG_UNUSED(cap);
    return NULL;
#endif
}

int mraf_platform_webrtc_tx_end(size_t len)
{
#if defined(CONFIG_MRAF_WEBRTC)
    return mraf_peer_tx_end(len);
#else
    ARG_UNUSED(len);
    return -1;
#endif
}

void mraf_platform_webrtc_set_rx(mraf_webrtc_rx_cb cb)
{
#if defined(CONFIG_MRAF_WEBRTC)
    mraf_peer_set_rx(cb);
#else
    ARG_UNUSED(cb);
#endif
}

void mraf_platform_webrtc_add_candidate(const char *candidate)
{
#if defined(CONFIG_MRAF_WEBRTC)
    mraf_peer_add_candidate(candidate);
#else
    ARG_UNUSED(candidate);
#endif
}

void mraf_platform_webrtc_ice_info(struct mraf_ice_info *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
#if defined(CONFIG_MRAF_WEBRTC)
    mraf_peer_ice_info(out);
#endif
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

// native_sim links the host libc and can read/write a file directly. A QEMU
// target such as qemu_x86 has no filesystem under the app, and pulling in
// picolibc FILE* would require the POSIX fd layer, so persistence degrades to
// in-memory defaults there.
#if defined(CONFIG_ARCH_POSIX) || defined(CONFIG_FILE_SYSTEM)

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

#else

bool mraf_storage_load(char *buf, size_t cap)
{
    (void)buf;
    (void)cap;
    return false;
}

bool mraf_storage_save(const char *json)
{
    (void)json;
    return false;
}

#endif

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
