// Platform abstraction: everything that differs between the ESP32 target and
// the native_sim test target lives behind these calls.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <time.h>

#include <ArduinoJson.h>

struct Config;

void mraf_platform_init(void);
void mraf_platform_start_network(const Config &cfg);
void mraf_platform_poll_network(void);

const char *mraf_platform_name(void);
bool mraf_platform_is_simulator(void);
bool mraf_platform_is_ap_mode(void);

void mraf_platform_net_info(bool *connected, char *ip, size_t ip_cap,
                            char *ssid, size_t ssid_cap, int *rssi);
void mraf_platform_wifi_scan(JsonDocument &doc);

bool mraf_platform_time_sync(void);
void mraf_platform_set_epoch(time_t t);
void mraf_set_timezone(const char *tz);

bool mraf_platform_rtc_begin(void);
bool mraf_platform_rtc_read(time_t *out);
void mraf_platform_rtc_write(time_t t);

void mraf_platform_led(bool on);
void mraf_platform_restart(void);

bool mraf_platform_mains_present(void);
bool mraf_platform_read_tank_full(bool active_low);
bool mraf_platform_read_sump_dry(bool active_low);
float mraf_platform_read_current_a(bool pump_commanded_on);
float mraf_platform_read_battery_v(void);

// Simulator-only controls exposed by /api/sim/* on native_sim.
void mraf_sim_init(void);
void mraf_sim_set_power(bool present);
void mraf_sim_set_sensors(bool tank_full, bool sump_dry);
void mraf_sim_set_current(float amps);
void mraf_sim_set_battery(float volts);
bool mraf_sim_power(void);
bool mraf_sim_tank_full(void);
bool mraf_sim_sump_dry(void);
float mraf_sim_current(void);
float mraf_sim_battery(void);
bool mraf_sim_current_is_auto(void);
void mraf_sim_set_current_auto(void);
