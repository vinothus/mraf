// Shared controller state and the APIs that the HTTP/CoAP front-ends call.
#pragma once

#include <time.h>

#include <ArduinoJson.h>

#include "mraf_compat.h"

#include <zephyr/kernel.h>

#include "model.h"
#include "controller.h"

extern Config g_cfg;
extern Controller g_ctrl;
extern struct k_mutex g_state_lock;

void mraf_state_init(void);
void mraf_sim_set_defaults(void);
void mraf_clock_init(time_t base);
time_t mraf_now(void);
void mraf_set_now(time_t t);
void mraf_advance_time(int64_t seconds);
int32_t mraf_time_speed(void);
void mraf_set_time_speed(int32_t speed);
bool mraf_time_valid(void);
void mraf_tick_catchup(void);
void mraf_apply_inputs_locked(time_t now);
void mraf_tick_reset(void);
void mraf_service_pulses(void);

// Serialize the full dashboard status document.
void mraf_build_status(JsonDocument &doc);
void mraf_build_remote_data(JsonObject data, time_t now);

// Apply a control command ("relay"/"pump"/"all"). Caller holds g_state_lock.
bool mraf_apply_control(JsonObject cmd, time_t now, String &error);
