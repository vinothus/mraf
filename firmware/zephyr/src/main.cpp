// MRAF farm controller - Zephyr RTOS application entry point.
#include <stdio.h>
#include <string.h>
#include <time.h>

#if !defined(CONFIG_ARCH_POSIX)
#include <zephyr/drivers/hwinfo.h>
#endif
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>

#include "api.h"
#include "config.h"
#include "mraf_platform.h"
#include "mraf_storage.h"
#include "mraf_state.h"
#include "remote.h"
#include "settings.h"

static void ensureDeviceId(void)
{
    if (g_cfg.device_id[0] != '\0') {
        return;
    }
#if defined(CONFIG_ARCH_POSIX)
    snprintf(g_cfg.device_id, sizeof(g_cfg.device_id), "SIM-%08X",
             sys_rand32_get());
#else
    uint8_t id[8] = {0};
    ssize_t n = hwinfo_get_device_id(id, sizeof(id));
    if (n >= 3) {
        snprintf(g_cfg.device_id, sizeof(g_cfg.device_id), "ESP32-%02X%02X%02X",
                 id[0], id[1], id[2]);
    } else {
        snprintf(g_cfg.device_id, sizeof(g_cfg.device_id), "SIM-%08X",
                 sys_rand32_get());
    }
#endif
    settingsSave(g_cfg);
}

static void updateStatusLed(void)
{
    bool connected = false;
    mraf_platform_net_info(&connected, nullptr, 0, nullptr, 0, nullptr);
    bool on;
    if (mraf_platform_is_ap_mode()) {
        on = ((k_uptime_get() / 250) % 2) == 0;
    } else if (connected) {
        on = ((k_uptime_get() / 1000) % 2) == 0;
    } else {
        on = ((k_uptime_get() / 120) % 2) == 0;
    }
    mraf_platform_led(on);
}

int main(void)
{
    mraf_platform_init();
    mraf_storage_init();
    mraf_state_init();

    ensureDeviceId();
    g_ctrl.applyConfig(g_cfg);

    time_t base = time(nullptr);
    time_t rtc = 0;
    bool rtc_ok = mraf_platform_rtc_begin() && mraf_platform_rtc_read(&rtc) &&
                  rtc > 1700000000;
    if (rtc_ok) {
        base = rtc;
        printk("RTC time: %s\n", localTimeString(rtc).c_str());
    } else if (!mraf_platform_is_simulator()) {
        printk("RTC not present or not set; waiting for NTP/manual time\n");
    }
    mraf_clock_init(base);
    if (mraf_platform_is_simulator()) {
        mraf_set_time_speed(60 * 1000);
    }
    mraf_tick_reset();

    mraf_remote_init();
    k_mutex_lock(&g_state_lock, K_FOREVER);
    g_ctrl.addEvent("system", "boot", mraf_now());
    k_mutex_unlock(&g_state_lock);

    mraf_platform_start_network(g_cfg);
    mraf_api_init();
    mraf_api_start();
    mraf_remote_start();

    printk("MRAF %s on %s, HTTP port %d, device %s\n", FW_VERSION,
           mraf_platform_name(), mraf_api_port(), g_cfg.device_id);

    while (1) {
        mraf_platform_poll_network();
        mraf_tick_catchup();
        mraf_service_pulses();
        updateStatusLed();
        k_msleep(10);
    }
    return 0;
}
