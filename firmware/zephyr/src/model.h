#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "config.h"
#include "schedule.h"

enum RunMode : uint8_t {
    MODE_AUTO = 0,
    MODE_ON = 1,
    MODE_OFF = 2
};

// Installation profile. The profile changes the labels and the default
// equipment list shown by both dashboards.
enum Profile : uint8_t {
    PROFILE_AGRI = 0,
    PROFILE_HOME = 1,
    PROFILE_GODOWN = 2,
    PROFILE_INDUSTRY = 3
};

enum Equipment : uint8_t {
    EQ_VALVE = 0,
    EQ_LIGHT = 1,
    EQ_MOTOR = 2,
    EQ_FAN = 3,
    EQ_PUMP = 4,
    EQ_SOCKET = 5,
    EQ_HEATER = 6,
    EQ_DOOR = 7,
    EQ_OTHER = 8
};

enum RepeatKind : uint8_t {
    REPEAT_DAILY = 0,
    REPEAT_WEEKLY = 1,
    REPEAT_MONTHLY = 2,
    REPEAT_YEARLY = 3
};

enum ScheduleAction : uint8_t {
    RACT_ON = 0,
    RACT_OFF = 1,
    RACT_AUTO = 2
};

struct ChannelConfig {
    char name[24];
    char zone[24];                      // zone / room / bay / line label
    uint8_t equipment;                  // Equipment
    bool pulse_mode;                    // momentary contactor pulse
    uint16_t pulse_on_ms;
    uint16_t pulse_off_ms;
    bool enabled;
    uint8_t days;                       // bit0=Mon ... bit6=Sun
    uint8_t nwin;
    SchedWindow win[MAX_WINDOWS];
    RunMode mode;
};

struct PumpConfig {
    char name[24];
    bool enabled;
    uint8_t days;
    uint8_t nwin;
    SchedWindow win[MAX_WINDOWS];
    RunMode mode;

    uint16_t min_off_sec;
    uint16_t max_run_min;
    uint16_t recovery_delay_sec;
    bool resume_after_outage;
    float dry_run_current_a;
    uint16_t dry_run_grace_sec;
    bool tank_full_active_low;
    bool sump_dry_active_low;
};

// Day / week / month / year recurring schedule. The same record can target
// any subset of the 12 relays and is evaluated alongside the daily windows.
struct RecurringSchedule {
    char name[24];
    bool enabled;
    uint8_t repeat;                     // RepeatKind
    uint8_t days;                       // weekly mask, bit0=Mon
    uint8_t day_of_month;               // monthly/yearly, 1..31
    uint8_t month;                      // yearly, 1..12
    uint16_t from_min;
    uint16_t to_min;
    uint16_t channel_mask;              // bit0 = relay 1 ... bit11 = relay 12
    uint8_t action;                     // ScheduleAction
};

struct Config {
    char device_name[40];
    char timezone[16];
    char auth_user[20];
    char auth_pass[32];

    char device_id[24];                 // stable identity sent to the server
    uint8_t profile;                    // Profile

    char wifi_ssid[33];
    char wifi_pass[65];
    char hostname[33];

    bool coap_enabled;
    char coap_host[64];
    uint16_t coap_port;
    char coap_path[32];
    uint16_t coap_interval_sec;         // server may override this
    char coap_token[48];
    char remote_user[20];               // credentials for the remote server
    char remote_pass[32];
    bool remote_register;               // ask the server to register us

    ChannelConfig channels[MAX_CHANNELS];
    PumpConfig pump;
    RecurringSchedule recurring[MAX_RECURRING];
    uint8_t nrecurring;
    uint16_t max_boost_min;
};

struct RelayRuntime {
    bool on;                            // logical state (motor running)
    bool output;                        // physical relay coil state
    bool schedule_active;
    bool recurring_active;
    bool boost_active;
    bool pulsing;
    unsigned long pulse_until_ms;
    time_t boost_until;
    time_t last_change;
};

struct PumpRuntime {
    bool on;
    bool want;
    bool schedule_active;
    bool boost_active;
    bool fault_latched;
    bool external_run;
    bool need_edge;             // resume_after_outage == false support
    bool tank_full;
    bool sump_dry;
    char fault[64];
    time_t boost_until;
    time_t run_start;
    time_t last_off;
    time_t last_change;
    float current_a;
};

struct EventRecord {
    time_t t;
    char src[8];
    char msg[64];
};
