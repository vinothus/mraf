// ESP32 platform implementation for the MRAF farm controller.
#include "mraf_platform.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/kernel.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/net/dns_sd.h>
#include <zephyr/net/hostname.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/sntp.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/settings/settings.h>
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
static struct net_mgmt_event_callback g_wifi_cb;
static struct k_sem g_ip_sem;
static K_THREAD_STACK_DEFINE(g_time_stack, 4096);
static struct k_thread g_time_thread;

static volatile bool g_sta_connected;
static volatile bool g_ap_enabled;
static char g_sta_ip[NET_IPV4_ADDR_LEN] = "0.0.0.0";
static char g_ap_ip[NET_IPV4_ADDR_LEN] = "192.168.4.1";
static char g_sta_ssid[33] = "";
static int g_rssi;
static bool g_time_synced;

static struct wifi_scan_result g_scan[16];
static volatile size_t g_scan_count;
static struct k_sem g_scan_done;

static float g_acs_zero_mv = 1250.0f;

#if !defined(CONFIG_MRAF_HTTP_PORT)
#define CONFIG_MRAF_HTTP_PORT 80
#endif

DNS_SD_REGISTER_TCP_SERVICE(mraf_http_mdns, "MRAF Farm Controller", "_http",
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
        flags = GPIO_OUTPUT;
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
            g_time_synced = true;
            printk("SNTP time: %s\n", localTimeString(t).c_str());
            break;
        }
        k_sleep(K_SECONDS(30));
    }
}

// --- ADC ------------------------------------------------------------------

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
    (void)settings_subsys_init();
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

// --- Wi-Fi / network ------------------------------------------------------

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
        }
    }
}

static void wifi_event_handler(struct net_mgmt_event_callback *cb,
                               uint64_t event, struct net_if *iface)
{
    ARG_UNUSED(iface);
    switch (event) {
    case NET_EVENT_WIFI_CONNECT_RESULT: {
        const struct wifi_status *st = (const struct wifi_status *)cb->info;
        g_sta_connected = st ? st->status == 0 : true;
        if (g_sta_connected && sta_iface) {
            net_dhcpv4_start(sta_iface);
        }
        break;
    }
    case NET_EVENT_WIFI_DISCONNECT_RESULT:
        g_sta_connected = false;
        break;
    case NET_EVENT_WIFI_AP_ENABLE_RESULT:
        g_ap_enabled = true;
        break;
    case NET_EVENT_WIFI_AP_DISABLE_RESULT:
        g_ap_enabled = false;
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
        }
        break;
    case NET_EVENT_IPV4_ADDR_DEL:
        if (iface == sta_iface) {
            g_sta_connected = false;
        }
        break;
    default:
        break;
    }
    update_net_info();
}

static void start_ap(const Config &cfg)
{
    if (!ap_iface) {
        return;
    }
    char ssid[33];
    const char *src = cfg.device_id[0] ? cfg.device_id : "MRAF";
    snprintf(ssid, sizeof(ssid), "%.28s", src);
    const char *psk = cfg.auth_pass[0] ? cfg.auth_pass : DEFAULT_AUTH_PASS;

    struct wifi_connect_req_params ap = {};
    ap.ssid = (const uint8_t *)ssid;
    ap.ssid_length = (uint8_t)strlen(ssid);
    ap.psk = (const uint8_t *)psk;
    ap.psk_length = (uint8_t)strlen(psk);
    ap.security = WIFI_SECURITY_TYPE_PSK;
    ap.channel = WIFI_CHANNEL_ANY;
    ap.band = WIFI_FREQ_BAND_2_4_GHZ;
    ap.timeout = 20;
    (void)net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, ap_iface, &ap, sizeof(ap));

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

static void start_sta(const Config &cfg)
{
    if (!sta_iface || cfg.wifi_ssid[0] == '\0') {
        return;
    }
    snprintf(g_sta_ssid, sizeof(g_sta_ssid), "%s", cfg.wifi_ssid);
    struct wifi_connect_req_params sta = {};
    sta.ssid = (const uint8_t *)cfg.wifi_ssid;
    sta.ssid_length = (uint8_t)strlen(cfg.wifi_ssid);
    if (cfg.wifi_pass[0]) {
        sta.psk = (const uint8_t *)cfg.wifi_pass;
        sta.psk_length = (uint8_t)strlen(cfg.wifi_pass);
        sta.security = WIFI_SECURITY_TYPE_PSK;
    } else {
        sta.security = WIFI_SECURITY_TYPE_NONE;
    }
    sta.channel = WIFI_CHANNEL_ANY;
    sta.band = WIFI_FREQ_BAND_2_4_GHZ;
    sta.timeout = 20;
    (void)net_mgmt(NET_REQUEST_WIFI_CONNECT, sta_iface, &sta, sizeof(sta));
}

void mraf_platform_init(void)
{
    mraf_set_timezone(DEFAULT_TIMEZONE);
    k_sem_init(&g_ip_sem, 0, 1);
    k_sem_init(&g_scan_done, 0, 1);

    if (device_is_ready(gpio0)) {
        gpio_pin_configure(gpio0, MRAF_STATUS_LED_PIN, GPIO_OUTPUT_INACTIVE);
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
    sta_iface = net_if_get_wifi_sta();
    ap_iface = net_if_get_wifi_sap();

    net_mgmt_init_event_callback(&g_wifi_cb, wifi_event_handler, WIFI_EVENT_MASK);
    net_mgmt_add_event_callback(&g_wifi_cb);
    net_hostname_set(cfg.device_id[0] ? cfg.device_id : "mraf",
                     strlen(cfg.device_id[0] ? cfg.device_id : "mraf"));

    start_ap(cfg);
    start_sta(cfg);

    k_thread_create(&g_time_thread, g_time_stack,
                    K_THREAD_STACK_SIZEOF(g_time_stack), time_sync_thread,
                    NULL, NULL, NULL, K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
    k_thread_name_set(&g_time_thread, "mraf_sntp");
}

void mraf_platform_poll_network(void)
{
    update_net_info();
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
                                 : (g_ap_enabled ? "MRAF-AP" : ""));
    }
    if (rssi) {
        *rssi = g_rssi;
    }
}

void mraf_platform_wifi_scan(JsonDocument &doc)
{
    JsonArray arr = doc["networks"].to<JsonArray>();
    if (!sta_iface) {
        return;
    }
    g_scan_count = 0;
    k_sem_reset(&g_scan_done);
    if (net_mgmt(NET_REQUEST_WIFI_SCAN, sta_iface, NULL, 0) != 0) {
        return;
    }
    (void)k_sem_take(&g_scan_done, K_SECONDS(8));
    for (size_t i = 0; i < g_scan_count; i++) {
        const struct wifi_scan_result *res = &g_scan[i];
        char name[WIFI_SSID_MAX_LEN + 1];
        size_t n = res->ssid_length;
        if (n > WIFI_SSID_MAX_LEN) {
            n = WIFI_SSID_MAX_LEN;
        }
        memcpy(name, res->ssid, n);
        name[n] = '\0';
        JsonObject o = arr.add<JsonObject>();
        o["ssid"] = name;
        o["rssi"] = res->rssi;
        o["secure"] = res->security != WIFI_SECURITY_TYPE_NONE;
    }
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
