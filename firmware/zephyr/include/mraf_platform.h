// Platform abstraction: everything that differs between the ESP32 target and
// the native_sim test target lives behind these calls.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <time.h>

#include <ArduinoJson.h>

struct Config;
struct WireGuardConfig;

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

// Bring the WireGuard overlay up (or tear it down) from a server-supplied
// configuration. Returns true when the tunnel was applied to the network
// stack, false when WireGuard support is not compiled in.
bool mraf_platform_wireguard_apply(const WireGuardConfig &cfg);
bool mraf_platform_wireguard_active(void);

// WebRTC data-channel transport. Signalling and command dispatch live in
// webrtc.cpp; only the DTLS/SCTP transport is platform specific.
//
// The browser is the offerer, so mraf_platform_webrtc_start() takes the remote
// SDP offer and must return the local answer SDP (with ICE candidates already
// gathered into it). The rx callback is invoked for every complete JSON
// message that arrives on the data channel.
typedef void (*mraf_webrtc_rx_cb)(const char *message);

bool mraf_platform_webrtc_available(void);
bool mraf_platform_webrtc_start(const char *offer_sdp, char *answer_sdp,
                                size_t answer_cap);
void mraf_platform_webrtc_stop(void);
bool mraf_platform_webrtc_open(void);
int mraf_platform_webrtc_send(const char *message);
char *mraf_platform_webrtc_tx_begin(size_t *cap);
int mraf_platform_webrtc_tx_end(size_t len);
void mraf_platform_webrtc_set_rx(mraf_webrtc_rx_cb cb);

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
