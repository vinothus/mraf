// ESP32 platform implementation for the MRAF farm controller.
#include "mraf_platform.h"
#include "webrtc_peer.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/net/dns_sd.h>
#include <zephyr/net/hostname.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/sntp.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/virtual.h>
#include <zephyr/net/virtual_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/wireguard.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/base64.h>
#if defined(CONFIG_ARCH_POSIX)
#include <zephyr/sys/reboot.h>
#else
#include <esp_system.h>
#endif
#include <zephyr/sys/timeutil.h>

#include "config.h"
#include "mraf_compat.h"
#include "mraf_hal.h"
#include "mraf_storage.h"
#include "mraf_state.h"
#include "settings.h"

#define WIFI_EVENT_MASK                                                        \
    (NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT |        \
     NET_EVENT_WIFI_SCAN_RESULT | NET_EVENT_WIFI_SCAN_DONE |                   \
     NET_EVENT_WIFI_AP_ENABLE_RESULT | NET_EVENT_WIFI_AP_DISABLE_RESULT |      \
     NET_EVENT_IPV4_ADDR_ADD | NET_EVENT_IPV4_ADDR_DEL)

static const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
static const struct device *gpio1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));
static const struct device *rtc_dev = DEVICE_DT_GET(DT_NODELABEL(ds3231_rtc));
#define MRAF_STATUS_LED_PIN 2

static const struct adc_dt_spec adc_battery =
    ADC_DT_SPEC_GET_BY_NAME(DT_PATH(zephyr_user), battery);
static const struct adc_dt_spec adc_current =
    ADC_DT_SPEC_GET_BY_NAME(DT_PATH(zephyr_user), current);

static char g_timezone[48] = "UTC0";

static struct net_if *sta_iface;
static struct net_if *ap_iface;
#if defined(CONFIG_WIFI_ESP32)
static struct net_mgmt_event_callback g_wifi_cb;
#endif
static struct k_sem g_ip_sem;
static K_THREAD_STACK_DEFINE(g_time_stack, 3072);
static struct k_thread g_time_thread;

static volatile bool g_sta_connected;
static volatile bool g_ap_enabled;
static bool g_ap_started;
static bool g_sta_configured;
static bool g_sta_connect_pending;
static int64_t g_sta_fallback_deadline_ms;
static int64_t g_sta_retry_deadline_ms;
static int64_t g_sta_connect_deadline_ms;
static enum wifi_iface_state g_sta_radio_state = WIFI_STATE_INACTIVE;
static const int64_t STA_FALLBACK_TIMEOUT_MS = 120000;
static const int64_t STA_RETRY_INTERVAL_MS = 30000;
static const int64_t CONNECT_ATTEMPT_TIMEOUT_MS = 30000;

static void schedule_sta_retry(int64_t delay_ms)
{
    int64_t deadline = k_uptime_get() + delay_ms;
    if (g_sta_retry_deadline_ms == 0 || deadline < g_sta_retry_deadline_ms) {
        g_sta_retry_deadline_ms = deadline;
    }
}
static char g_sta_ip[NET_IPV4_ADDR_LEN] = "0.0.0.0";
static char g_ap_ip[NET_IPV4_ADDR_LEN] = "192.168.4.1";
static char g_sta_ssid[33] = "";
static char g_sta_pass[65] = "";
static char g_ap_ssid[33] = "MRAF";
static int g_rssi;
static bool g_time_synced;

static struct wifi_scan_result g_scan[16];
static volatile size_t g_scan_count;
static struct k_sem g_scan_done;

static float g_acs_zero_mv = 1250.0f;

#if !defined(CONFIG_MRAF_HTTP_PORT)
#define CONFIG_MRAF_HTTP_PORT 80
#endif

DNS_SD_REGISTER_TCP_SERVICE(mraf_http_mdns, "Neer Kaavalan", "_http",
                            "local", DNS_SD_EMPTY_TXT, CONFIG_MRAF_HTTP_PORT);

// --- helpers --------------------------------------------------------------

static const struct device *gpio_for(uint8_t pin)
{
    return pin < 32 ? gpio0 : gpio1;
}

static uint32_t gpio_mask(uint8_t pin)
{
    return BIT(pin & 31);
}

void mraf_gpio_config(uint8_t pin, uint8_t mode)
{
    const struct device *dev = gpio_for(pin);
    gpio_flags_t flags = GPIO_INPUT;
    if (mode == OUTPUT) {
        flags = GPIO_OUTPUT | GPIO_INPUT;
    } else if (mode == INPUT_PULLUP) {
        flags = GPIO_INPUT | GPIO_PULL_UP;
    }
    gpio_pin_configure(dev, pin & 31, flags);
    if (mode == OUTPUT) {
        gpio_pin_set(dev, pin & 31, 1);
    }
}

void mraf_gpio_write(uint8_t pin, uint8_t level)
{
    gpio_pin_set(gpio_for(pin), pin & 31, level ? 1 : 0);
}

int mraf_gpio_read(uint8_t pin)
{
    return gpio_pin_get_raw(gpio_for(pin), pin & 31) > 0 ? HIGH : LOW;
}

unsigned long mraf_gpio_millis(void)
{
    return k_uptime_get_32();
}

static bool read_float_pin(uint8_t pin, bool active_low)
{
    int raw = mraf_gpio_read(pin);
    return active_low ? (raw == LOW) : (raw == HIGH);
}

// --- time -----------------------------------------------------------------

void mraf_set_timezone(const char *tz)
{
    if (!tz) {
        tz = "UTC0";
    }
    snprintf(g_timezone, sizeof(g_timezone), "%s", tz);
}

static int32_t tz_offset_seconds(void)
{
    const char *p = g_timezone;
    while (*p && !(*p == '+' || *p == '-' || (*p >= '0' && *p <= '9'))) {
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

static bool rtc_to_epoch(const struct rtc_time *rt, time_t *out)
{
    int year = rt->tm_year + 1900;
    if (year < 2023 || year > 2200 || rt->tm_mon < 0 || rt->tm_mon > 11 ||
        rt->tm_mday < 1 || rt->tm_mday > 31) {
        return false;
    }
    struct tm tm = {};
    tm.tm_sec = rt->tm_sec;
    tm.tm_min = rt->tm_min;
    tm.tm_hour = rt->tm_hour;
    tm.tm_mday = rt->tm_mday;
    tm.tm_mon = rt->tm_mon;
    tm.tm_year = rt->tm_year;
    int64_t utc = timeutil_timegm64(&tm);
    if (utc < 1700000000) {
        return false;
    }
    *out = (time_t)(utc - tz_offset_seconds());
    return true;
}

bool mraf_platform_rtc_begin(void)
{
    return device_is_ready(rtc_dev);
}

bool mraf_platform_rtc_read(time_t *out)
{
    if (!device_is_ready(rtc_dev)) {
        return false;
    }
    struct rtc_time rt = {};
    if (rtc_get_time(rtc_dev, &rt) != 0) {
        return false;
    }
    return rtc_to_epoch(&rt, out);
}

void mraf_platform_rtc_write(time_t t)
{
    if (!device_is_ready(rtc_dev)) {
        return;
    }
    struct tm tm;
    localtime_r(&t, &tm);
    struct rtc_time rt = {};
    rt.tm_sec = tm.tm_sec;
    rt.tm_min = tm.tm_min;
    rt.tm_hour = tm.tm_hour;
    rt.tm_wday = tm.tm_wday;
    rt.tm_mday = tm.tm_mday;
    rt.tm_mon = tm.tm_mon;
    rt.tm_year = tm.tm_year;
    rtc_set_time(rtc_dev, &rt);
}

bool mraf_platform_time_sync(void)
{
    return g_time_synced;
}

void mraf_platform_set_epoch(time_t t)
{
    mraf_set_now(t);
    mraf_platform_rtc_write(t);
    mraf_platform_persistent_time_write(t);
}

static void time_sync_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    if (k_sem_take(&g_ip_sem, K_FOREVER) != 0) {
        return;
    }
    while (!g_time_synced) {
        struct sntp_time ts;
        if (sntp_simple(CONFIG_MRAF_NTP_SERVER, 5000, &ts) == 0 &&
            ts.seconds > 1700000000ULL) {
            time_t t = (time_t)ts.seconds;
            mraf_set_now(t);
            mraf_platform_rtc_write(t);
            mraf_platform_persistent_time_write(t);
            g_time_synced = true;
            printk("SNTP time: %s\n", localTimeString(t).c_str());
            break;
        }
        k_sleep(K_SECONDS(30));
    }
}

// --- ADC ------------------------------------------------------------------

// Zephyr's Xtensa boot path leaves CPENABLE to the SoC reset code, and the
// ESP32 reset path never enables coprocessor 0 (the FPU). Any thread that
// touches a float dies with EXCCAUSE 32. Turn it on as early as possible.
static int mraf_enable_fpu(void)
{
    uint32_t cp;
    __asm__ volatile("rsr.cpenable %0" : "=r"(cp));
    cp |= BIT(0);
    __asm__ volatile("wsr.cpenable %0" ::"r"(cp));
    return 0;
}
SYS_INIT(mraf_enable_fpu, PRE_KERNEL_1, 0);

static int read_adc_mv(const struct adc_dt_spec *spec, int32_t *mv_out)
{
    if (!adc_is_ready_dt(spec)) {
        return -ENODEV;
    }
    // The ESP32 ADC driver rejects hardware oversampling, so average in
    // software over a small burst.
    int64_t sum = 0;
    const int count = 16;
    for (int i = 0; i < count; i++) {
        int16_t sample = 0;
        struct adc_sequence sequence = {};
        if (adc_sequence_init_dt(spec, &sequence) != 0) {
            return -EINVAL;
        }
        sequence.buffer = &sample;
        sequence.buffer_size = sizeof(sample);
        int err = adc_read_dt(spec, &sequence);
        if (err != 0) {
            return err;
        }
        sum += sample;
    }
    int32_t mv = (int32_t)(sum / count);
    int err = adc_raw_to_millivolts_dt(spec, &mv);
    if (err != 0) {
        return err;
    }
    *mv_out = mv;
    return 0;
}

float mraf_platform_read_battery_v(void)
{
    int32_t mv = 0;
    if (read_adc_mv(&adc_battery, &mv) != 0) {
        return 0.0f;
    }
    return (mv / 1000.0f) * BATT_DIVIDER_RATIO;
}

float mraf_platform_read_current_a(bool pump_commanded_on)
{
    ARG_UNUSED(pump_commanded_on);
    int32_t mv = 0;
    if (read_adc_mv(&adc_current, &mv) != 0) {
        return 0.0f;
    }
    float delta = (float)mv - g_acs_zero_mv;
    if (delta < 0.0f) {
        delta = -delta;
    }
    return delta / ACS_SENSITIVITY_MV_PER_A;
}

// --- persistence ----------------------------------------------------------

void mraf_storage_init(void)
{
    int err = settings_subsys_init();
    if (err) {
        printk("settings: subsystem init failed (%d)\n", err);
    }
}

bool mraf_storage_load(char *buf, size_t cap)
{
    if (!buf || cap < 2) {
        return false;
    }
    ssize_t n = settings_load_one("mraf/settings", buf, cap - 1);
    if (n <= 0) {
        return false;
    }
    buf[n] = '\0';
    return true;
}

bool mraf_storage_save(const char *json)
{
    if (!json) {
        return false;
    }
    return settings_save_one("mraf/settings", json, strlen(json)) == 0;
}

bool mraf_platform_persistent_time_read(time_t *out)
{
    char buf[32];
    ssize_t n = settings_load_one("mraf/time", buf, sizeof(buf) - 1);
    if (n <= 0) {
        return false;
    }
    buf[n] = '\0';
    errno = 0;
    char *end = nullptr;
    long long value = strtoll(buf, &end, 10);
    if (errno != 0 || end == buf || *end != '\0' || value <= 1700000000LL) {
        return false;
    }
    if (out) {
        *out = (time_t)value;
    }
    return true;
}

void mraf_platform_persistent_time_write(time_t t)
{
    if ((long long)t <= 1700000000LL) {
        return;
    }
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lld", (long long)t);
    if (n > 0) {
        settings_save_one("mraf/time", buf, (size_t)n);
    }
}

// --- Wi-Fi / network ------------------------------------------------------

// Poll the radio directly. Several Wi-Fi notifications (softAP start, scan
// done) have been observed to never reach the net_mgmt event callback even
// though the radio itself is running, so the event handler is only a fast
// path: once a second we ask the driver for the real state and reconcile.
static void poll_radio_state(void)
{
    static int64_t last_poll;
    int64_t now = k_uptime_get();
    if (now - last_poll < 1000) {
        return;
    }
    last_poll = now;

    struct wifi_iface_status st = {};

    if (sta_iface && net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, sta_iface,
                              &st, sizeof(st)) == 0) {
        g_sta_radio_state = (enum wifi_iface_state)st.state;
        bool up = st.state == WIFI_STATE_COMPLETED;
        if (up && !g_sta_connected) {
            // Covers driver-side auto-reconnects whose connect event was
            // never delivered, and the first association in general.
            printk("Wi-Fi: associated with \"%s\" (state %s)\n",
                   g_sta_ssid, wifi_state_txt((enum wifi_iface_state)st.state));
            net_dhcpv4_start(sta_iface);
        } else if (!up && g_sta_connected) {
            printk("Wi-Fi: link down (state %s)\n",
                   wifi_state_txt((enum wifi_iface_state)st.state));
        }
        g_sta_connected = up;
        if (up) {
            g_sta_fallback_deadline_ms = 0;
            g_rssi = st.rssi;
            if (st.ssid_len > 0 && st.ssid_len < sizeof(g_sta_ssid)) {
                memcpy(g_sta_ssid, st.ssid, st.ssid_len);
                g_sta_ssid[st.ssid_len] = '\0';
            }
        }
    }

    if (ap_iface && !g_ap_enabled) {
        struct wifi_iface_status ap = {};
        if (net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, ap_iface,
                     &ap, sizeof(ap)) == 0 &&
            ap.state != WIFI_STATE_INACTIVE &&
            ap.state != WIFI_STATE_INTERFACE_DISABLED) {
            g_ap_enabled = true;
            printk("Wi-Fi AP: \"%s\" active on channel %u, open http://192.168.4.1/\n",
                   g_ap_ssid, ap.channel);
        }
    }
}

static void update_net_info(void)
{
    struct net_if *iface = g_sta_connected ? sta_iface : ap_iface;
    if (!iface) {
        return;
    }
    struct net_in_addr *addr =
        net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
    if (addr && (g_sta_connected || g_ap_enabled)) {
        net_addr_ntop(AF_INET, addr, g_sta_connected ? g_sta_ip : g_ap_ip,
                      NET_IPV4_ADDR_LEN);
    }
    if (g_sta_connected && sta_iface) {
        struct wifi_iface_status status = {};
        if (net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, sta_iface, &status,
                      sizeof(status)) == 0) {
            g_rssi = status.rssi;
            // The driver can reconnect on its own, so trust the radio for the
            // SSID we are actually associated with.
            if (status.ssid_len > 0 &&
                status.ssid_len < sizeof(g_sta_ssid)) {
                memcpy(g_sta_ssid, status.ssid, status.ssid_len);
                g_sta_ssid[status.ssid_len] = '\0';
            }
        }
    }
}

// Console feedback when the device joins an outside Wi-Fi network.
static void print_sta_ip(void)
{
    static char prev[NET_IPV4_ADDR_LEN] = "";

    if (!g_sta_connected) {
        return;
    }
    update_net_info();
    if (g_sta_ip[0] != '\0' && strcmp(g_sta_ip, prev) != 0 &&
        strcmp(g_sta_ip, "0.0.0.0") != 0) {
        printk("Wi-Fi: connected to \"%s\", IP address %s\n",
               g_sta_ssid, g_sta_ip);
        strncpy(prev, g_sta_ip, sizeof(prev) - 1);
        prev[sizeof(prev) - 1] = '\0';
    }
}

#if defined(CONFIG_WIFI_ESP32)
static void wifi_event_handler(struct net_mgmt_event_callback *cb,
                               uint64_t event, struct net_if *iface)
{
    ARG_UNUSED(iface);
    if (event != NET_EVENT_WIFI_SCAN_RESULT) {
        printk("Wi-Fi net event: 0x%llx\n", (unsigned long long)event);
    }
    switch (event) {
    case NET_EVENT_WIFI_CONNECT_RESULT: {
        const struct wifi_status *st = (const struct wifi_status *)cb->info;
        bool ok = st ? st->status == 0 : true;
        g_sta_connect_pending = false;
        g_sta_connect_deadline_ms = 0;
        g_sta_connected = ok;
        if (ok && sta_iface) {
            g_sta_fallback_deadline_ms = 0;
            g_sta_retry_deadline_ms = 0;
            net_dhcpv4_start(sta_iface);
            printk("Wi-Fi: associated with \"%s\", waiting for IP...\n",
                   g_sta_ssid);
        } else if (!ok) {
            printk("Wi-Fi: connect to \"%s\" failed (status %d: %s)\n",
                   g_sta_ssid, st ? st->status : -1,
                   st ? wifi_conn_status_txt((enum wifi_conn_status)st->status)
                      : "no detail");
            if (g_sta_configured) {
                // Keep the original STA-only fallback window; don't start AP
                // immediately on the first failure.
                if (!g_ap_started && g_sta_fallback_deadline_ms == 0) {
                    g_sta_fallback_deadline_ms =
                        k_uptime_get() + STA_FALLBACK_TIMEOUT_MS;
                }
                schedule_sta_retry(STA_RETRY_INTERVAL_MS);
            }
        }
        break;
    }
    case NET_EVENT_WIFI_DISCONNECT_RESULT: {
        const struct wifi_status *st = (const struct wifi_status *)cb->info;
        if (g_sta_connected) {
            printk("Wi-Fi: disconnected from \"%s\"\n", g_sta_ssid);
        } else if (g_sta_configured) {
            printk("Wi-Fi: disconnect event for \"%s\"\n", g_sta_ssid);
        }
        if (st) {
            printk("Wi-Fi: disconnect detail status=%d (%s)\n", st->status,
                   wifi_disconn_reason_txt((enum wifi_disconn_reason)st->status));
        }
        g_sta_connected = false;
        g_sta_connect_pending = false;
        g_sta_connect_deadline_ms = 0;
        if (g_sta_configured) {
            if (!g_ap_started && g_sta_fallback_deadline_ms == 0) {
                g_sta_fallback_deadline_ms =
                    k_uptime_get() + STA_FALLBACK_TIMEOUT_MS;
            }
            schedule_sta_retry(STA_RETRY_INTERVAL_MS);
        }
        break;
    }
    case NET_EVENT_WIFI_AP_ENABLE_RESULT:
        if (!g_ap_enabled) {
            printk("Wi-Fi AP: \"%s\" active, open http://192.168.4.1/\n", g_ap_ssid);
        }
        g_ap_enabled = true;
        g_ap_started = true;
        break;
    case NET_EVENT_WIFI_AP_DISABLE_RESULT:
        g_ap_enabled = false;
        g_ap_started = false;
        break;
    case NET_EVENT_WIFI_SCAN_RESULT: {
        const struct wifi_scan_result *res =
            (const struct wifi_scan_result *)cb->info;
        if (res && g_scan_count < ARRAY_SIZE(g_scan)) {
            g_scan[g_scan_count++] = *res;
        }
        break;
    }
    case NET_EVENT_WIFI_SCAN_DONE:
        k_sem_give(&g_scan_done);
        break;
    case NET_EVENT_IPV4_ADDR_ADD:
        if (iface == sta_iface) {
            g_sta_connected = true;
            k_sem_give(&g_ip_sem);
            print_sta_ip();
        }
        break;
    case NET_EVENT_IPV4_ADDR_DEL:
        if (iface == sta_iface) {
            if (g_sta_connected) {
                printk("Wi-Fi: lost IP address, reconnecting\n");
            }
            g_sta_connected = false;
        }
        break;
    default:
        break;
    }
    update_net_info();
}

static void start_ap(const char *device_id, const char *auth_pass)
{
    if (g_ap_started) {
        return;
    }
    if (!ap_iface) {
        printk("Wi-Fi AP: softAP interface missing - AP not started\n");
        return;
    }
    char ssid[33];
    const char *src = (device_id && device_id[0]) ? device_id : "MRAF";
    snprintf(ssid, sizeof(ssid), "%.28s", src);
    snprintf(g_ap_ssid, sizeof(g_ap_ssid), "%s", ssid);
    const char *psk = (auth_pass && auth_pass[0]) ? auth_pass : DEFAULT_AUTH_PASS;
    size_t psk_len = strlen(psk);
    if (psk_len < 8 || psk_len > 63) {
        printk("Wi-Fi AP: auth password length %u invalid for WPA2, using default\n",
               (unsigned)psk_len);
        psk = DEFAULT_AUTH_PASS;
        psk_len = strlen(psk);
    }

    struct wifi_connect_req_params ap = {};
    ap.ssid = (const uint8_t *)ssid;
    ap.ssid_length = (uint8_t)strlen(ssid);
    ap.psk = (const uint8_t *)psk;
    ap.psk_length = (uint8_t)psk_len;
    ap.security = WIFI_SECURITY_TYPE_PSK;
    ap.channel = WIFI_CHANNEL_ANY;
    ap.band = WIFI_FREQ_BAND_2_4_GHZ;
    ap.timeout = 20;
    int ap_ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, ap_iface, &ap, sizeof(ap));
    if (ap_ret != 0) {
        printk("Wi-Fi AP: enable failed (%d) - AP not started\n", ap_ret);
        return;
    }
    g_ap_started = true;
    printk("Wi-Fi AP: \"%s\" starting at 192.168.4.1\n", ssid);

    struct net_in_addr ap_addr;
    struct net_in_addr netmask;
    struct net_in_addr pool;
    net_addr_pton(AF_INET, "192.168.4.1", &ap_addr);
    net_addr_pton(AF_INET, "255.255.255.0", &netmask);
    net_addr_pton(AF_INET, "192.168.4.10", &pool);
    net_if_ipv4_addr_add(ap_iface, &ap_addr, NET_ADDR_MANUAL, 0);
    net_if_ipv4_set_netmask_by_addr(ap_iface, &ap_addr, &netmask);
    net_if_ipv4_set_gw(ap_iface, &ap_addr);
    net_dhcpv4_server_start(ap_iface, &pool);
}

static void start_ap_fallback(void)
{
    if (g_ap_started || !g_sta_configured) {
        return;
    }
    char dev_id[24] = "MRAF";
    char auth_pass[32];
    snprintf(auth_pass, sizeof(auth_pass), "%s", DEFAULT_AUTH_PASS);
    k_mutex_lock(&g_state_lock, K_FOREVER);
    if (g_cfg.device_id[0]) {
        snprintf(dev_id, sizeof(dev_id), "%s", g_cfg.device_id);
    }
    if (g_cfg.auth_pass[0]) {
        snprintf(auth_pass, sizeof(auth_pass), "%s", g_cfg.auth_pass);
    }
    k_mutex_unlock(&g_state_lock);
    printk("Wi-Fi: STA not connected, starting AP+STA fallback\n");
    start_ap(dev_id, auth_pass);
    schedule_sta_retry(STA_RETRY_INTERVAL_MS);
}

static void apply_wifi_regulatory_domain(void)
{
    if (!sta_iface) {
        return;
    }
    const char *code = CONFIG_MRAF_WIFI_COUNTRY;
    if (!code || strlen(code) < 2) {
        code = "IN";
    }
    bool was_up = net_if_is_admin_up(sta_iface);

    struct wifi_reg_domain reg = {};
    reg.oper = WIFI_MGMT_SET;
    /* force disables 802.11d so channels 12/13 are actively scanned instead
     * of only passively heard while the radio is idle.
     */
    reg.force = true;
    reg.country_code[0] = (uint8_t)code[0];
    reg.country_code[1] = (uint8_t)code[1];
    int ret = net_mgmt(NET_REQUEST_WIFI_REG_DOMAIN, sta_iface, &reg, sizeof(reg));
    printk("Wi-Fi: regulatory domain %c%c %s (err %d)\n", code[0], code[1],
           ret == 0 ? "set" : "not set", ret);

    /* Set the country before enabling the interface so the first scan uses
     * the expanded channel list, not the world-safe 1-11 default.
     */
    if (!was_up) {
        int up_ret = net_if_up(sta_iface);
        if (up_ret != 0) {
            printk("Wi-Fi: STA interface up failed (err %d)\n", up_ret);
        }
    }
}

#if defined(CONFIG_MRAF_WIFI_BOOT_SCAN)
static void log_startup_scan(void)
{
    if (!sta_iface || g_sta_ssid[0] == '\0') {
        return;
    }

    printk("Wi-Fi scan: scanning 2.4 GHz for \"%s\"...\n", g_sta_ssid);
    g_scan_count = 0;
    k_sem_reset(&g_scan_done);
    int ret = net_mgmt(NET_REQUEST_WIFI_SCAN, sta_iface, NULL, 0);
    if (ret != 0 && ret != -EINPROGRESS && ret != -EBUSY) {
        printk("Wi-Fi scan: request failed (err %d)\n", ret);
        return;
    }

    int wait = k_sem_take(&g_scan_done, K_SECONDS(8));
    size_t n = g_scan_count < ARRAY_SIZE(g_scan) ? g_scan_count : ARRAY_SIZE(g_scan);
    size_t hidden = 0;
    bool found = false;
    size_t target_len = strlen(g_sta_ssid);

    for (size_t i = 0; i < n; i++) {
        const struct wifi_scan_result *res = &g_scan[i];
        if (res->ssid_length == 0) {
            hidden++;
            continue;
        }
        char name[WIFI_SSID_MAX_LEN + 1];
        size_t len = res->ssid_length;
        if (len > WIFI_SSID_MAX_LEN) {
            len = WIFI_SSID_MAX_LEN;
        }
        memcpy(name, res->ssid, len);
        name[len] = '\0';
        printk("Wi-Fi scan: \"%s\" ch=%u rssi=%d %s\n", name, res->channel,
               res->rssi,
               res->security == WIFI_SECURITY_TYPE_NONE ? "open" : "secure");
        if (res->ssid_length == target_len &&
            memcmp(res->ssid, g_sta_ssid, target_len) == 0) {
            found = true;
        }
    }

    printk("Wi-Fi scan: %u result(s), %u hidden%s\n", (unsigned)n,
           (unsigned)hidden, wait == 0 ? "" : " (timed out)");
    if (found) {
        printk("Wi-Fi scan: target \"%s\" is visible on 2.4 GHz\n", g_sta_ssid);
    } else {
        printk("Wi-Fi scan: target \"%s\" NOT visible on 2.4 GHz "
               "(check 5 GHz-only, 2.4 GHz disabled, hidden SSID or spelling)\n",
               g_sta_ssid);
    }
}
#endif /* CONFIG_MRAF_WIFI_BOOT_SCAN */

static const char *wifi_connect_req_str(int ret)
{
    if (ret == 0) {
        return "in progress";
    }
    if (ret == -EALREADY) {
        return "already connecting";
    }
    if (ret == -EBUSY) {
        return "busy";
    }
    return "request failed";
}

static int sta_send_connect(void)
{
    struct wifi_connect_req_params sta = {};
    sta.ssid = (const uint8_t *)g_sta_ssid;
    sta.ssid_length = (uint8_t)strlen(g_sta_ssid);
    if (g_sta_pass[0]) {
        sta.psk = (const uint8_t *)g_sta_pass;
        sta.psk_length = (uint8_t)strlen(g_sta_pass);
        sta.security = WIFI_SECURITY_TYPE_PSK;
    } else {
        sta.security = WIFI_SECURITY_TYPE_NONE;
    }
    sta.channel = WIFI_CHANNEL_ANY;
    sta.band = WIFI_FREQ_BAND_2_4_GHZ;
    sta.timeout = 20;
    int sta_ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, sta_iface, &sta, sizeof(sta));
    if (sta_ret == 0 || sta_ret == -EALREADY || sta_ret == -EINPROGRESS) {
        g_sta_connect_pending = true;
        g_sta_connect_deadline_ms = k_uptime_get() + CONNECT_ATTEMPT_TIMEOUT_MS;
    }
    return sta_ret;
}

static void start_sta(const Config &cfg)
{
    if (!sta_iface) {
        printk("Wi-Fi STA: station interface missing\n");
        return;
    }
    if (cfg.wifi_ssid[0] == '\0') {
        printk("Wi-Fi STA: no network configured, staying in AP mode\n");
        return;
    }
    snprintf(g_sta_ssid, sizeof(g_sta_ssid), "%s", cfg.wifi_ssid);
    snprintf(g_sta_pass, sizeof(g_sta_pass), "%s", cfg.wifi_pass);
#if defined(CONFIG_MRAF_WIFI_BOOT_SCAN)
    log_startup_scan();
#endif
    int sta_ret = sta_send_connect();
    printk("Wi-Fi STA: connecting to \"%s\" (%s, err %d)\n", g_sta_ssid,
           wifi_connect_req_str(sta_ret), sta_ret);
}

static void retry_sta_connect(void)
{
    if (!g_sta_configured || g_sta_connected || !sta_iface || g_sta_connect_pending) {
        return;
    }
    int sta_ret = sta_send_connect();
    printk("Wi-Fi STA: retry connect to \"%s\" (%s, err %d)\n", g_sta_ssid,
           wifi_connect_req_str(sta_ret), sta_ret);
    if (sta_ret == -EALREADY) {
        (void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, sta_iface, NULL, 0);
        g_sta_connect_pending = false;
        g_sta_connect_deadline_ms = 0;
        schedule_sta_retry(5000);
    } else if (sta_ret == -EBUSY) {
        schedule_sta_retry(10000);
    } else {
        schedule_sta_retry(STA_RETRY_INTERVAL_MS);
    }
}
#endif /* CONFIG_WIFI_ESP32 */

void mraf_platform_init(void)
{
#if !defined(CONFIG_ARCH_POSIX)
    printk("Reset reason: %d\n", (int)esp_reset_reason());
#endif
    mraf_set_timezone(DEFAULT_TIMEZONE);
    k_sem_init(&g_ip_sem, 0, 1);
    k_sem_init(&g_scan_done, 0, 1);

    if (device_is_ready(gpio0)) {
        int led_ret = gpio_pin_configure(gpio0, MRAF_STATUS_LED_PIN,
                                         GPIO_OUTPUT_INACTIVE);
        if (led_ret != 0) {
            printk("status LED: GPIO%d configure failed (%d), no blink\n",
                   MRAF_STATUS_LED_PIN, led_ret);
        } else {
            printk("status LED: GPIO%d ready (AP mode = fast blink, "
                   "Wi-Fi = slow blink)\n", MRAF_STATUS_LED_PIN);
        }
    } else {
        printk("status LED: gpio0 device not ready, no blink\n");
    }
    // Relay board is active-low: drive all outputs high (off) at boot.
    for (int i = 0; i < MAX_CHANNELS; i++) {
        mraf_gpio_config(RELAY_PINS[i], OUTPUT);
        mraf_gpio_write(RELAY_PINS[i], HIGH);
    }
    mraf_gpio_config(PUMP_RELAY_PIN, OUTPUT);
    mraf_gpio_write(PUMP_RELAY_PIN, HIGH);
    mraf_gpio_config(MAINS_SENSE_PIN, INPUT);
    mraf_gpio_config(TANK_FULL_PIN, INPUT);
    mraf_gpio_config(SUMP_DRY_PIN, INPUT);

    if (adc_is_ready_dt(&adc_battery)) {
        adc_channel_setup_dt(&adc_battery);
    }
    if (adc_is_ready_dt(&adc_current)) {
        adc_channel_setup_dt(&adc_current);
    }
    int32_t zero = 0;
    if (read_adc_mv(&adc_current, &zero) == 0 && zero > 200 && zero < 2500) {
        g_acs_zero_mv = (float)zero;
    }
}

const char *mraf_platform_name(void)
{
    return "esp32";
}

bool mraf_platform_is_simulator(void)
{
    return false;
}

bool mraf_platform_is_ap_mode(void)
{
    return !g_sta_connected;
}

void mraf_platform_start_network(const Config &cfg)
{
#if defined(CONFIG_WIFI_ESP32)
    sta_iface = net_if_get_wifi_sta();
    ap_iface = net_if_get_wifi_sap();

    net_mgmt_init_event_callback(&g_wifi_cb, wifi_event_handler, WIFI_EVENT_MASK);
    net_mgmt_add_event_callback(&g_wifi_cb);
#endif
    net_hostname_set(cfg.device_id[0] ? cfg.device_id : "mraf",
                     strlen(cfg.device_id[0] ? cfg.device_id : "mraf"));

#if defined(CONFIG_WIFI_ESP32)
    g_sta_configured = cfg.wifi_ssid[0] != '\0';
    g_ap_started = false;
    g_sta_fallback_deadline_ms = 0;
    g_sta_retry_deadline_ms = 0;
    g_sta_ssid[0] = '\0';
    g_sta_pass[0] = '\0';
    g_sta_connect_pending = false;
    g_sta_connect_deadline_ms = 0;
    g_sta_radio_state = WIFI_STATE_INACTIVE;

    apply_wifi_regulatory_domain();
    if (g_sta_configured) {
        start_sta(cfg);
        g_sta_fallback_deadline_ms = k_uptime_get() + STA_FALLBACK_TIMEOUT_MS;
        printk("Wi-Fi: trying STA \"%s\" first, AP+STA fallback in %d ms if not connected\n",
               cfg.wifi_ssid, (int)STA_FALLBACK_TIMEOUT_MS);
    } else {
        printk("Wi-Fi: no STA SSID configured, starting AP for provisioning\n");
        start_ap(cfg.device_id, cfg.auth_pass);
    }
#endif

    k_thread_create(&g_time_thread, g_time_stack,
                    K_THREAD_STACK_SIZEOF(g_time_stack), time_sync_thread,
                    NULL, NULL, NULL, K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
    k_thread_name_set(&g_time_thread, "mraf_sntp");
}

void mraf_platform_poll_network(void)
{
    poll_radio_state();
    int64_t now = k_uptime_get();
    if (g_sta_configured && !g_sta_connected && !g_ap_started &&
        g_sta_fallback_deadline_ms > 0 &&
        now >= g_sta_fallback_deadline_ms) {
        start_ap_fallback();
        g_sta_fallback_deadline_ms = 0;
    }
    if (g_sta_configured && !g_sta_connected && sta_iface &&
        g_sta_connect_pending && g_sta_connect_deadline_ms > 0 &&
        now >= g_sta_connect_deadline_ms) {
        printk("Wi-Fi: connect attempt timed out (state %s), forcing disconnect\n",
               wifi_state_txt(g_sta_radio_state));
        g_sta_connect_pending = false;
        g_sta_connect_deadline_ms = 0;
        (void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, sta_iface, NULL, 0);
        schedule_sta_retry(5000);
    }
    if (g_sta_configured && !g_sta_connected && !g_sta_connect_pending &&
        g_sta_retry_deadline_ms > 0 &&
        now >= g_sta_retry_deadline_ms) {
        retry_sta_connect();
    }
    update_net_info();
    print_sta_ip();

    // Serial heartbeat while the network is not usable yet, so a stalled
    // radio bring-up is visible on the console instead of dead silence.
    static int64_t last_net_log;
    if (now - last_net_log > 15000) {
        last_net_log = now;
        if (!g_ap_enabled && !g_sta_connected) {
            printk("net: no softAP, no station yet, state=%s pending=%d (t=%lld s)\n",
                   wifi_state_txt(g_sta_radio_state), (int)g_sta_connect_pending,
                   now / 1000);
        } else if (g_sta_configured && !g_sta_connected && g_ap_enabled) {
            printk("net: AP fallback active, STA \"%s\" state=%s pending=%d (t=%lld s)\n",
                   g_sta_ssid, wifi_state_txt(g_sta_radio_state),
                   (int)g_sta_connect_pending, now / 1000);
        }
    }
}

void mraf_platform_net_info(bool *connected, char *ip, size_t ip_cap,
                            char *ssid, size_t ssid_cap, int *rssi)
{
    if (connected) {
        *connected = g_sta_connected || g_ap_enabled;
    }
    if (ip && ip_cap) {
        snprintf(ip, ip_cap, "%s", g_sta_connected ? g_sta_ip : g_ap_ip);
    }
    if (ssid && ssid_cap) {
        snprintf(ssid, ssid_cap, "%s",
                 g_sta_connected ? g_sta_ssid
                                 : (g_ap_enabled ? g_ap_ssid : ""));
    }
    if (rssi) {
        *rssi = g_rssi;
    }
}

void mraf_platform_wifi_scan(JsonDocument &doc)
{
    JsonArray arr = doc["networks"].to<JsonArray>();
    // The ESP32 radio only scans the 2.4 GHz band; surface that so the UI can
    // explain why 5 GHz-only SSIDs never show up.
    doc["radio"] = "2.4 GHz";
    if (!sta_iface) {
        doc["error"] = "Wi-Fi station interface not ready";
        return;
    }

    g_scan_count = 0;
    k_sem_reset(&g_scan_done);
    int ret = net_mgmt(NET_REQUEST_WIFI_SCAN, sta_iface, NULL, 0);
    if (ret == -EINPROGRESS || ret == -EBUSY) {
        // A scan is already running (or the driver is busy). Its results still
        // arrive through the event callback below, so just wait for them.
    } else if (ret != 0) {
        // The station may have been mid-connect when the scan was asked for;
        // give the radio a beat and retry once.
        k_sleep(K_MSEC(500));
        g_scan_count = 0;
        k_sem_reset(&g_scan_done);
        ret = net_mgmt(NET_REQUEST_WIFI_SCAN, sta_iface, NULL, 0);
        if (ret != 0 && ret != -EINPROGRESS && ret != -EBUSY) {
            char err[48];
            snprintf(err, sizeof(err), "scan failed (err %d), radio busy", ret);
            doc["error"] = err;
            return;
        }
    }

    // Wait for the scan to finish; results keep arriving even past the
    // timeout, so report whatever was collected.
    int wait = k_sem_take(&g_scan_done, K_SECONDS(8));
    if (wait != 0 && g_scan_count == 0) {
        // The request was accepted but neither results nor the done event
        // arrived: the radio never reported back.
        doc["error"] = "scan timed out with no results (radio did not report)";
    }

    // Collapse duplicates (same SSID heard on several APs/channels) keeping
    // the strongest signal, and skip hidden networks.
    size_t n = g_scan_count < ARRAY_SIZE(g_scan) ? g_scan_count : ARRAY_SIZE(g_scan);
    size_t used = 0;
    for (size_t i = 0; i < n; i++) {
        const struct wifi_scan_result *res = &g_scan[i];
        if (res->ssid_length == 0) {
            continue;
        }
        bool dup = false;
        for (size_t j = 0; j < used; j++) {
            const struct wifi_scan_result *keep = &g_scan[j];
            if (keep->ssid_length == res->ssid_length &&
                memcmp(keep->ssid, res->ssid, res->ssid_length) == 0) {
                dup = true;
                if (res->rssi > keep->rssi) {
                    g_scan[j] = *res;
                }
                break;
            }
        }
        if (!dup && used != i) {
            g_scan[used] = *res;
        }
        if (!dup) {
            used++;
        }
    }

    for (size_t i = 0; i < used; i++) {
        const struct wifi_scan_result *res = &g_scan[i];
        char name[WIFI_SSID_MAX_LEN + 1];
        size_t len = res->ssid_length;
        if (len > WIFI_SSID_MAX_LEN) {
            len = WIFI_SSID_MAX_LEN;
        }
        memcpy(name, res->ssid, len);
        name[len] = '\0';
        JsonObject o = arr.add<JsonObject>();
        o["ssid"] = name;
        o["rssi"] = res->rssi;
        o["secure"] = res->security != WIFI_SECURITY_TYPE_NONE;
        o["channel"] = res->channel;
        o["band"] = res->band == WIFI_FREQ_BAND_5_GHZ ? "5 GHz"
                  : res->band == WIFI_FREQ_BAND_6_GHZ ? "6 GHz"
                  : "2.4 GHz";
    }
    doc["count"] = (int)used;
    doc["scanned"] = (int)n;
}

void mraf_platform_led(bool on)
{
    if (device_is_ready(gpio0)) {
        gpio_pin_set(gpio0, MRAF_STATUS_LED_PIN, on ? 1 : 0);
    }
}

void mraf_platform_restart(void)
{
#if defined(CONFIG_ARCH_POSIX)
    sys_reboot(SYS_REBOOT_COLD);
#else
    esp_restart();
#endif
}

// --- WireGuard overlay ----------------------------------------------------
// The server pushes a complete tunnel description over CoAP. When the build
// enables the native Zephyr WireGuard service we install it into the network
// stack; otherwise the configuration is stored but not applied.

#if defined(CONFIG_WIREGUARD)

static bool g_wg_active;

struct wg_iface_search {
    struct net_if *iface;
};

static void wg_iface_cb(struct net_if *iface, void *user_data)
{
    struct wg_iface_search *s = (struct wg_iface_search *)user_data;
    if (s->iface) {
        return;
    }
    if (net_if_l2(iface) != &NET_L2_GET_NAME(VIRTUAL)) {
        return;
    }
    if (!(net_virtual_get_iface_capabilities(iface) & VIRTUAL_INTERFACE_VPN)) {
        return;
    }
    s->iface = iface;
}

static bool wg_add_address(struct net_if *iface, const char *spec)
{
    struct sockaddr_storage addr = {};
    struct sockaddr *paddr = (struct sockaddr *)&addr;
    uint8_t mask_len = 0;
    if (!net_ipaddr_parse_mask(spec, strlen(spec), paddr, &mask_len)) {
        return false;
    }
    if (paddr->sa_family == AF_INET) {
        struct sockaddr_in *a4 = (struct sockaddr_in *)paddr;
        if (!net_if_ipv4_addr_add(iface, &a4->sin_addr, NET_ADDR_MANUAL, 0)) {
            return false;
        }
        struct sockaddr_in mask = {};
        if (net_mask_len_to_netmask(AF_INET, mask_len, (struct sockaddr *)&mask) < 0) {
            return false;
        }
        return net_if_ipv4_set_netmask_by_addr(iface, &a4->sin_addr, &mask.sin_addr);
    }
    if (paddr->sa_family == AF_INET6) {
        struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)paddr;
        if (!net_if_ipv6_addr_add(iface, &a6->sin6_addr, NET_ADDR_MANUAL, 0)) {
            return false;
        }
        struct in6_addr net6 = {};
        net_ipv6_addr_prefix_mask((uint8_t *)&a6->sin6_addr, (uint8_t *)&net6, mask_len);
        return net_if_ipv6_prefix_add(iface, &net6, mask_len, 0xffffffff);
    }
    return false;
}

static bool wg_add_allowed_ips(struct wireguard_peer_config *peer, const char *spec)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", spec);
    bool found = false;
    char *save = nullptr;
    for (char *tok = strtok_r(buf, ", ", &save); tok; tok = strtok_r(nullptr, ", ", &save)) {
        struct sockaddr_storage addr = {};
        struct sockaddr *paddr = (struct sockaddr *)&addr;
        uint8_t mask_len = 0;
        if (!net_ipaddr_parse_mask(tok, strlen(tok), paddr, &mask_len)) {
            continue;
        }
        for (int i = 0; i < WIREGUARD_MAX_SRC_IPS; i++) {
            if (peer->allowed_ip[i].is_valid) {
                continue;
            }
            if (paddr->sa_family == AF_INET) {
                struct sockaddr_in *a4 = (struct sockaddr_in *)paddr;
                memcpy(&peer->allowed_ip[i].addr.in_addr, &a4->sin_addr,
                       sizeof(struct in_addr));
                peer->allowed_ip[i].addr.family = AF_INET;
            } else if (paddr->sa_family == AF_INET6) {
                struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)paddr;
                memcpy(&peer->allowed_ip[i].addr.in6_addr, &a6->sin6_addr,
                       sizeof(struct in6_addr));
                peer->allowed_ip[i].addr.family = AF_INET6;
            } else {
                break;
            }
            peer->allowed_ip[i].is_valid = true;
            peer->allowed_ip[i].mask_len = mask_len;
            found = true;
            break;
        }
    }
    return found;
}

static bool wg_resolve_endpoint(const char *endpoint, struct sockaddr_storage *out)
{
    char host[80];
    char port[8] = "51820";
    snprintf(host, sizeof(host), "%s", endpoint ? endpoint : "");
    char *colon = strrchr(host, ':');
    if (colon && strchr(host, ']') == nullptr) {
        *colon = '\0';
        snprintf(port, sizeof(port), "%s", colon + 1);
    } else if (colon && colon[1] != '\0') {
        snprintf(port, sizeof(port), "%s", colon + 1);
        char *close = strchr(host, ']');
        if (close) {
            *close = '\0';
        }
    }
    if (host[0] == '[') {
        memmove(host, host + 1, strlen(host));
    }
    struct zsock_addrinfo hints = {};
    struct zsock_addrinfo *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (zsock_getaddrinfo(host, port, &hints, &res) != 0 || !res) {
        return false;
    }
    memcpy(out, res->ai_addr, res->ai_addrlen);
    zsock_freeaddrinfo(res);
    return true;
}

bool mraf_platform_wireguard_apply(const WireGuardConfig &cfg)
{
    struct wg_iface_search search = {};
    net_if_foreach(wg_iface_cb, &search);
    if (!search.iface) {
        printk("WireGuard: no VPN-capable interface available\n");
        return false;
    }
    struct net_if *iface = search.iface;

    uint8_t private_key[NET_VIRTUAL_MAX_PUBLIC_KEY_LEN];
    size_t olen = 0;
    if (base64_decode(private_key, sizeof(private_key), &olen, cfg.private_key,
                      strlen(cfg.private_key)) < 0) {
        printk("WireGuard: invalid private key\n");
        return false;
    }
    struct virtual_interface_req_params params = {};
    params.private_key.data = private_key;
    params.private_key.len = sizeof(private_key);
    int ret = net_mgmt(NET_REQUEST_VIRTUAL_INTERFACE_SET_PRIVATE_KEY, iface,
                       &params, sizeof(params));
    memset(private_key, 0, sizeof(private_key));
    if (ret < 0) {
        printk("WireGuard: cannot set private key (%d)\n", ret);
        return false;
    }

    if (cfg.address[0] && !wg_add_address(iface, cfg.address)) {
        printk("WireGuard: cannot add address %s\n", cfg.address);
    }

    struct wireguard_peer_config peer = {};
    peer.public_key = cfg.peer_public_key;

    static uint8_t psk[32];
    if (cfg.peer_preshared_key[0]) {
        size_t plen = 0;
        if (base64_decode(psk, sizeof(psk), &plen, cfg.peer_preshared_key,
                          strlen(cfg.peer_preshared_key)) >= 0) {
            peer.preshared_key = psk;
        }
    }

    if (!wg_add_allowed_ips(&peer, cfg.peer_allowed_ips)) {
        printk("WireGuard: no usable allowed IPs\n");
        return false;
    }

    if (!wg_resolve_endpoint(cfg.peer_endpoint, &peer.endpoint_ip)) {
        printk("WireGuard: cannot resolve endpoint %s\n", cfg.peer_endpoint);
        return false;
    }

    peer.keepalive_interval = cfg.persistent_keepalive;

    struct net_if *peer_iface = nullptr;
    ret = wireguard_peer_add(&peer, &peer_iface);
    if (ret < 0) {
        printk("WireGuard: cannot add peer (%d)\n", ret);
        return false;
    }

    // The keepalive after peer_add does not start a handshake until traffic is
    // routed into the tunnel, so send one probe datagram to the first allowed
    // address to bring the tunnel up (Zephyr issue #123993).
    int fd = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd >= 0) {
        struct sockaddr_in probe = {};
        probe.sin_family = AF_INET;
        probe.sin_port = htons(9);
        if (peer.allowed_ip[0].is_valid && peer.allowed_ip[0].addr.family == AF_INET) {
            memcpy(&probe.sin_addr, &peer.allowed_ip[0].addr.in_addr,
                   sizeof(struct in_addr));
            (void)zsock_sendto(fd, "m", 1, 0, (struct sockaddr *)&probe, sizeof(probe));
        }
        zsock_close(fd);
    }

    g_wg_active = true;
    printk("WireGuard: peer %d installed on interface %d\n", ret,
           peer_iface ? net_if_get_by_iface(peer_iface) : -1);
    return true;
}

bool mraf_platform_wireguard_active(void)
{
    return g_wg_active;
}

#else // !CONFIG_WIREGUARD

bool mraf_platform_wireguard_apply(const WireGuardConfig &cfg)
{
    ARG_UNUSED(cfg);
    printk("WireGuard: built without CONFIG_WIREGUARD; config stored only\n");
    return false;
}

bool mraf_platform_wireguard_active(void)
{
    return false;
}

#endif // CONFIG_WIREGUARD

#if defined(CONFIG_MRAF_WEBRTC)

// WebRTC data-channel transport for the ESP32 target.
//
// The portable peer in webrtc_peer.cpp (ICE-lite STUN + mbedTLS DTLS + the
// minimal SCTP association) is shared with the native_sim build, so the code
// exercised against a real browser on the host is the same code that runs on
// the ESP32. The signalling client in webrtc.cpp owns the SDP offer/answer
// exchange and passes the remote offer straight through.
bool mraf_platform_webrtc_available(void)
{
    return mraf_peer_available();
}

bool mraf_platform_webrtc_start(const char *offer_sdp, char *answer_sdp,
                                size_t answer_cap)
{
    return mraf_peer_start(offer_sdp, answer_sdp, answer_cap);
}

void mraf_platform_webrtc_stop(void)
{
    mraf_peer_stop();
}

bool mraf_platform_webrtc_open(void)
{
    return mraf_peer_open();
}

int mraf_platform_webrtc_send(const char *message)
{
    return mraf_peer_send(message);
}

char *mraf_platform_webrtc_tx_begin(size_t *cap)
{
    return mraf_peer_tx_begin(cap);
}

int mraf_platform_webrtc_tx_end(size_t len)
{
    return mraf_peer_tx_end(len);
}

void mraf_platform_webrtc_set_rx(mraf_webrtc_rx_cb cb)
{
    mraf_peer_set_rx(cb);
}

void mraf_platform_webrtc_add_candidate(const char *candidate)
{
    mraf_peer_add_candidate(candidate);
}

void mraf_platform_webrtc_ice_info(struct mraf_ice_info *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    mraf_peer_ice_info(out);
}

#else // !CONFIG_MRAF_WEBRTC

bool mraf_platform_webrtc_available(void)
{
    return false;
}

bool mraf_platform_webrtc_start(const char *offer_sdp, char *answer_sdp,
                                size_t answer_cap)
{
    ARG_UNUSED(offer_sdp);
    if (answer_sdp && answer_cap) {
        answer_sdp[0] = '\0';
    }
    return false;
}

void mraf_platform_webrtc_stop(void)
{
}

bool mraf_platform_webrtc_open(void)
{
    return false;
}

int mraf_platform_webrtc_send(const char *message)
{
    ARG_UNUSED(message);
    return -1;
}

char *mraf_platform_webrtc_tx_begin(size_t *cap)
{
    ARG_UNUSED(cap);
    return NULL;
}

int mraf_platform_webrtc_tx_end(size_t len)
{
    ARG_UNUSED(len);
    return -1;
}

void mraf_platform_webrtc_set_rx(mraf_webrtc_rx_cb cb)
{
    ARG_UNUSED(cb);
}

void mraf_platform_webrtc_add_candidate(const char *candidate)
{
    ARG_UNUSED(candidate);
}

void mraf_platform_webrtc_ice_info(struct mraf_ice_info *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
    }
}

#endif // CONFIG_MRAF_WEBRTC

bool mraf_platform_mains_present(void)
{
    return read_float_pin(MAINS_SENSE_PIN, !MAINS_SENSE_ACTIVE_HIGH);
}

bool mraf_platform_read_tank_full(bool active_low)
{
    return read_float_pin(TANK_FULL_PIN, active_low);
}

bool mraf_platform_read_sump_dry(bool active_low)
{
    return read_float_pin(SUMP_DRY_PIN, active_low);
}

// --- simulator stubs ------------------------------------------------------
// The shared state/API code references these unconditionally. The ESP32 has
// real hardware, so the simulator controls are inert.

void mraf_sim_init(void) {}
void mraf_sim_set_power(bool present) { ARG_UNUSED(present); }
void mraf_sim_set_sensors(bool tank_full, bool sump_dry)
{
    ARG_UNUSED(tank_full);
    ARG_UNUSED(sump_dry);
}
void mraf_sim_set_current(float amps) { ARG_UNUSED(amps); }
void mraf_sim_set_battery(float volts) { ARG_UNUSED(volts); }
bool mraf_sim_power(void) { return true; }
bool mraf_sim_tank_full(void) { return false; }
bool mraf_sim_sump_dry(void) { return false; }
float mraf_sim_current(void) { return 0.0f; }
float mraf_sim_battery(void) { return mraf_platform_read_battery_v(); }
bool mraf_sim_current_is_auto(void) { return true; }
void mraf_sim_set_current_auto(void) {}
