// Native host test for the ESP32 controller state machine.
// Build with: make test-cpp
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "../firmware/mraf/controller.h"

int g_mraf_pin_state[64];

static int failures = 0;

static void check(bool cond, const char *what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        failures++;
    }
}

// Local time, independent of the host timezone. day 0 = Monday 2 Nov 2026.
static time_t at(int day, int hour, int minute) {
    struct tm t;
    std::memset(&t, 0, sizeof(t));
    t.tm_year = 2026 - 1900;
    t.tm_mon = 10;
    t.tm_mday = 2 + day;
    t.tm_hour = hour;
    t.tm_min = minute;
    t.tm_sec = 0;
    t.tm_isdst = -1;
    return mktime(&t);
}

static Config baseConfig() {
    Config c;
    std::memset(&c, 0, sizeof(c));
    std::strncpy(c.device_name, "test", sizeof(c.device_name) - 1);
    c.max_boost_min = 180;

    for (uint8_t i = 0; i < MAX_CHANNELS; i++) {
        c.channels[i].mode = MODE_AUTO;
        c.channels[i].days = 127;
    }

    PumpConfig &p = c.pump;
    p.enabled = true;
    p.days = 127;
    p.mode = MODE_AUTO;
    p.nwin = 1;
    p.win[0].on_min = 6 * 60;
    p.win[0].off_min = 6 * 60 + 30;
    p.min_off_sec = 0;
    p.max_run_min = 120;
    p.recovery_delay_sec = 0;
    p.resume_after_outage = true;
    p.dry_run_current_a = 1.5f;
    p.dry_run_grace_sec = 20;
    p.tank_full_active_low = false;
    p.sump_dry_active_low = false;
    return c;
}

static void tick(Controller &c, time_t now, bool power = true,
                 bool tank_full = false, bool sump_dry = false,
                 float current_a = 7.0f) {
    c.tick(now, power, tank_full, sump_dry, current_a, 12.8f);
}

static void testScheduledStartStop() {
    Controller c;
    c.begin(baseConfig());
    const time_t t0 = at(0, 6, 0);

    tick(c, t0 - 1, true, false, false, 0.0f);
    check(!c.pump().on, "pump idle before schedule");

    tick(c, t0, true, false, false, 0.0f);
    check(c.pump().on, "pump starts at schedule start");
    check(c.pump().want, "pump want true in window");
    check(g_mraf_pin_state[PUMP_RELAY_PIN] == LOW, "pump relay active low");

    tick(c, t0 + 1, true, false, false, 7.0f);
    check(std::strcmp(c.pumpStateName(t0 + 1), "RUNNING") == 0,
          "pump state RUNNING with current");

    tick(c, at(0, 6, 30), true, false, false, 7.0f);
    check(!c.pump().on, "pump stops at schedule end");
    check(g_mraf_pin_state[PUMP_RELAY_PIN] == HIGH, "pump relay released");
    check(c.eventCount() > 0, "events recorded");
}

static void testFloatInterlocks() {
    Config cfg = baseConfig();
    const time_t t0 = at(0, 6, 0);

    Controller tank;
    tank.begin(cfg);
    tick(tank, t0, true, true, false, 0.0f);
    check(!tank.pump().on, "tank full blocks pump");
    check(!tank.pump().want, "tank full clears want");
    check(tank.pump().tank_full, "tank full state recorded");

    Controller sump;
    sump.begin(cfg);
    tick(sump, t0, true, false, true, 0.0f);
    check(!sump.pump().on, "sump dry blocks pump");
    check(!sump.pump().want, "sump dry clears want");
    check(sump.pump().sump_dry, "sump dry state recorded");
}

static void testDryRunTripAndReset() {
    Controller c;
    c.begin(baseConfig());
    const time_t t0 = at(0, 6, 0);

    tick(c, t0, true, false, false, 0.0f);
    check(c.pump().on, "pump starts before dry run test");
    tick(c, t0 + 1, true, false, false, 7.0f);

    for (int s = 2; s <= 21; s++) tick(c, t0 + s, true, false, false, 0.0f);
    check(c.pump().fault_latched, "dry run latches fault");
    check(!c.pump().on, "pump off after dry run trip");
    check(std::strstr(c.pump().fault, "dry run") != nullptr,
          "dry run fault text");
    check(std::strcmp(c.pumpStateName(t0 + 21), "FAULT") == 0,
          "pump state FAULT");

    c.resetPumpFault(t0 + 22);
    check(!c.pump().fault_latched, "fault reset clears latch");
    tick(c, t0 + 22, true, false, false, 0.0f);
    check(c.pump().on, "pump restarts after fault reset");
}

static void testMaxRunTrip() {
    Config cfg = baseConfig();
    cfg.pump.max_run_min = 1;
    cfg.pump.dry_run_current_a = 0.0f;

    Controller c;
    c.begin(cfg);
    const time_t t0 = at(0, 6, 0);

    tick(c, t0, true, false, false, 0.0f);
    tick(c, t0 + 1, true, false, false, 7.0f);
    tick(c, t0 + 61, true, false, false, 7.0f);
    check(c.pump().fault_latched, "max run latches fault");
    check(!c.pump().on, "pump off after max run trip");
    check(std::strstr(c.pump().fault, "max run") != nullptr,
          "max run fault text");
}

static void testOutageRecoveryWithResume() {
    Config cfg = baseConfig();
    cfg.pump.recovery_delay_sec = 30;
    cfg.pump.resume_after_outage = true;

    Controller c;
    c.begin(cfg);
    const time_t t0 = at(0, 6, 0);

    tick(c, t0, true, false, false, 0.0f);
    tick(c, t0 + 1, true, false, false, 7.0f);
    check(c.pump().on, "pump running before outage");

    tick(c, t0 + 10, false, false, false, 0.0f);
    check(!c.pump().on, "pump off during outage");
    check(!c.powerPresent(), "power state lost");
    check(c.outageSeconds(t0 + 40) == 30, "outage duration");

    tick(c, t0 + 40, true, false, false, 0.0f);
    check(c.powerPresent(), "power restored");
    check(!c.pump().on, "pump waits for recovery delay");
    check(c.pump().want, "pump wants to run after recovery");

    tick(c, t0 + 70, true, false, false, 0.0f);
    check(c.pump().on, "pump starts after recovery delay");
}

static void testOutageRequiresScheduleEdge() {
    Config cfg = baseConfig();
    cfg.pump.resume_after_outage = false;

    Controller c;
    c.begin(cfg);
    const time_t t0 = at(0, 6, 0);

    tick(c, t0, true, false, false, 0.0f);
    tick(c, t0 + 1, true, false, false, 7.0f);
    tick(c, t0 + 10, false, false, false, 0.0f);
    check(c.pump().need_edge, "outage sets need_edge");

    tick(c, t0 + 20, true, false, false, 0.0f);
    check(!c.pump().on, "no resume without schedule edge");
    check(!c.pump().want, "want suppressed by need_edge");

    tick(c, at(0, 6, 30), true, false, false, 0.0f);
    check(!c.pump().need_edge, "leaving window clears need_edge");

    tick(c, at(1, 6, 0), true, false, false, 0.0f);
    check(c.pump().on, "next schedule edge starts pump");
}

static void testMinimumOffTime() {
    Config cfg = baseConfig();
    cfg.pump.mode = MODE_ON;
    cfg.pump.nwin = 0;
    cfg.pump.min_off_sec = 300;

    Controller c;
    c.begin(cfg);
    const time_t t0 = at(0, 6, 0);

    tick(c, t0, true, false, false, 0.0f);
    check(c.pump().on, "manual pump starts");

    c.setPumpMode(MODE_OFF, 0, t0 + 60);
    check(!c.pump().on, "manual off stops pump");
    c.setPumpMode(MODE_ON, 0, t0 + 60);

    tick(c, t0 + 60, true, false, false, 0.0f);
    check(!c.pump().on, "minimum off blocks restart");
    tick(c, t0 + 360, true, false, false, 0.0f);
    check(c.pump().on, "restart allowed after minimum off");
}

static void testRelayScheduleManualAndBoost() {
    Config cfg = baseConfig();
    cfg.pump.enabled = false;
    ChannelConfig &ch = cfg.channels[0];
    ch.enabled = true;
    ch.mode = MODE_AUTO;
    ch.nwin = 1;
    ch.win[0].on_min = 18 * 60;
    ch.win[0].off_min = 18 * 60 + 30;

    Controller c;
    c.begin(cfg);
    const time_t t0 = at(0, 18, 0);

    tick(c, t0, true, false, false, 0.0f);
    check(c.relay(0).on, "relay starts at schedule start");
    check(g_mraf_pin_state[RELAY_PINS[0]] == LOW, "relay pin active low");

    tick(c, t0 + 1, true, false, false, 0.0f);
    tick(c, at(0, 18, 30), true, false, false, 0.0f);
    check(!c.relay(0).on, "relay stops at schedule end");

    c.setRelayMode(0, MODE_AUTO, 1, at(0, 18, 30));
    tick(c, at(0, 18, 30) + 30, true, false, false, 0.0f);
    check(c.relay(0).on, "relay boost turns on");
    tick(c, at(0, 18, 30) + 60, true, false, false, 0.0f);
    check(!c.relay(0).on, "relay boost expires");

    c.setRelayMode(0, MODE_ON, 0, at(0, 19, 0));
    check(c.config().channels[0].mode == MODE_ON, "relay manual on");
    c.allOff(at(0, 19, 1));
    check(!c.relay(0).on, "all off drops relay");
    check(c.config().channels[0].mode == MODE_OFF, "all off sets mode");
    c.allAuto(at(0, 19, 2));
    check(c.config().channels[0].mode == MODE_AUTO, "all auto sets mode");
}

static void testPulseRelay() {
    Config cfg = baseConfig();
    cfg.pump.enabled = false;
    ChannelConfig &ch = cfg.channels[0];
    ch.enabled = true;
    ch.mode = MODE_AUTO;
    ch.pulse_mode = true;
    ch.pulse_on_ms = 100;
    ch.pulse_off_ms = 50;
    ch.nwin = 1;
    ch.win[0].on_min = 18 * 60;
    ch.win[0].off_min = 18 * 60 + 30;

    Controller c;
    c.begin(cfg);
    const time_t t0 = at(0, 18, 0);
    c.setPulseClock(1000);

    tick(c, t0, true, false, false, 0.0f);
    check(c.relay(0).on, "pulse relay logical on");
    check(c.relay(0).output, "pulse relay physically on");
    check(c.relay(0).pulsing, "pulse relay pulsing");

    c.setPulseClock(1099);
    c.servicePulses();
    check(c.relay(0).output, "pulse still on before duration");

    c.setPulseClock(1100);
    c.servicePulses();
    check(!c.relay(0).output, "pulse releases after duration");
    check(!c.relay(0).pulsing, "pulse flag cleared");
    check(c.relay(0).on, "logical state stays on after pulse");

    // An explicit pulse_ms overrides the configured on duration.
    c.setPulseClock(2000);
    c.setRelayMode(0, MODE_ON, 0, t0 + 1, 250);
    check(c.relay(0).output, "manual pulse starts output");
    c.setPulseClock(2249);
    c.servicePulses();
    check(c.relay(0).output, "manual pulse holds until pulse_ms");
    c.setPulseClock(2250);
    c.servicePulses();
    check(!c.relay(0).output, "manual pulse ends at pulse_ms");

    // Releasing a pulse-mode relay uses the off pulse for a contactor.
    c.setRelayMode(0, MODE_OFF, 0, t0 + 2);
    check(!c.relay(0).on, "pulse relay logical off");
    check(c.relay(0).output, "off pulse drives output");
    c.setPulseClock(2250 + 50);
    c.servicePulses();
    check(!c.relay(0).output, "off pulse releases");
}

static void testRecurringRelay() {
    Config cfg = baseConfig();
    cfg.pump.enabled = false;
    ChannelConfig &ch = cfg.channels[0];
    ch.enabled = false;
    ch.mode = MODE_AUTO;
    ch.nwin = 0;

    cfg.nrecurring = 1;
    RecurringSchedule &rs = cfg.recurring[0];
    std::memset(&rs, 0, sizeof(rs));
    std::strncpy(rs.name, "soak", sizeof(rs.name) - 1);
    rs.enabled = true;
    rs.repeat = REPEAT_WEEKLY;
    rs.days = 1;
    rs.day_of_month = 1;
    rs.month = 1;
    rs.from_min = 5 * 60;
    rs.to_min = 5 * 60 + 15;
    rs.channel_mask = 1;
    rs.action = RACT_ON;

    Controller c;
    c.begin(cfg);
    tick(c, at(0, 5, 5), true, false, false, 0.0f);
    check(c.relay(0).on, "recurring weekly rule turns relay on");
    check(c.relay(0).recurring_active, "recurring flag set");

    tick(c, at(0, 5, 15), true, false, false, 0.0f);
    check(!c.relay(0).on, "recurring rule ends at to time");
    check(!c.relay(0).recurring_active, "recurring flag cleared");

    tick(c, at(1, 5, 5), true, false, false, 0.0f);
    check(!c.relay(0).on, "weekly rule ignores Tuesday");

    // An off rule wins when it overlaps an on rule.
    cfg.nrecurring = 2;
    RecurringSchedule &off = cfg.recurring[1];
    std::memset(&off, 0, sizeof(off));
    std::strncpy(off.name, "off", sizeof(off.name) - 1);
    off.enabled = true;
    off.repeat = REPEAT_DAILY;
    off.from_min = 5 * 60;
    off.to_min = 5 * 60 + 15;
    off.channel_mask = 1;
    off.action = RACT_OFF;

    Controller c2;
    c2.begin(cfg);
    tick(c2, at(0, 5, 5), true, false, false, 0.0f);
    check(!c2.relay(0).on, "recurring off rule wins");
    check(c2.relay(0).recurring_active, "off rule still marks active");
}

int main() {
    testScheduledStartStop();
    testFloatInterlocks();
    testDryRunTripAndReset();
    testMaxRunTrip();
    testOutageRecoveryWithResume();
    testOutageRequiresScheduleEdge();
    testMinimumOffTime();
    testRelayScheduleManualAndBoost();
    testPulseRelay();
    testRecurringRelay();

    if (failures) {
        std::printf("%d controller test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("all controller tests passed\n");
    return EXIT_SUCCESS;
}
