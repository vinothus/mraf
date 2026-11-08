#include "controller.h"

#include <string.h>
#include <time.h>

static void copyEventStr(char *dst, size_t n, const char *src) {
    if (!dst || n == 0) return;
    if (!src) src = "";
    strncpy(dst, src, n - 1);
    dst[n - 1] = '\0';
}

static const char *runModeText(RunMode m) {
    switch (m) {
        case MODE_ON: return "on";
        case MODE_OFF: return "off";
        default: return "auto";
    }
}

static void schedDate(time_t t, SchedDate &d) {
    struct tm tm;
    localtime_r(&t, &tm);
    d.year = (uint16_t)(tm.tm_year + 1900);
    d.month = (uint8_t)(tm.tm_mon + 1);
    d.day = (uint8_t)tm.tm_mday;
    d.weekday = (uint8_t)((tm.tm_wday + 6) % 7);   // Mon=0
}

void Controller::writeRelayPin(uint8_t pin, bool on) const {
    if (RELAY_ACTIVE_LOW) digitalWrite(pin, on ? LOW : HIGH);
    else digitalWrite(pin, on ? HIGH : LOW);
}

void Controller::setRelayPhysical(uint8_t i, bool on) {
    writeRelayPin(RELAY_PINS[i], on);
    relay_[i].output = on;
}

void Controller::startPulse(uint8_t i, uint16_t ms) {
    if (ms < 20) ms = 20;
    relay_[i].pulsing = true;
    relay_[i].pulse_until_ms = pulse_clock_ms_ + (unsigned long)ms;
    setRelayPhysical(i, true);
}

void Controller::setRelayLogical(uint8_t i, bool on, time_t now) {
    RelayRuntime &rt = relay_[i];
    rt.on = on;
    rt.last_change = now;
    if (cfg_.channels[i].pulse_mode) {
        uint16_t ms = on ? cfg_.channels[i].pulse_on_ms
                         : cfg_.channels[i].pulse_off_ms;
        startPulse(i, ms);
    } else {
        setRelayPhysical(i, on);
    }
}

void Controller::servicePulses() {
    for (uint8_t i = 0; i < MAX_CHANNELS; i++) {
        RelayRuntime &rt = relay_[i];
        if (rt.pulsing &&
            (long)(pulse_clock_ms_ - rt.pulse_until_ms) >= 0) {
            rt.pulsing = false;
            setRelayPhysical(i, false);
        }
    }
}

void Controller::setPumpOutput(bool on, time_t now) {
    writeRelayPin(PUMP_RELAY_PIN, on);
    pump_.on = on;
    pump_.last_change = now;
}

void Controller::begin(const Config &cfg) {
    cfg_ = cfg;
    memset(relay_, 0, sizeof(relay_));
    memset(&pump_, 0, sizeof(pump_));
    event_head_ = 0;
    event_count_ = 0;
    power_ = true;
    outage_start_ = 0;
    restored_at_ = 0;
    battery_v_ = 0.0f;
    last_batt_log_ = 0;
    pulse_clock_ms_ = 0;

    // Park every output in the OFF state before enabling the driver, then
    // enable it. This avoids a relay pulse at boot.
    for (uint8_t i = 0; i < MAX_CHANNELS; i++) {
        uint8_t pin = RELAY_PINS[i];
        digitalWrite(pin, RELAY_ACTIVE_LOW ? HIGH : LOW);
        pinMode(pin, OUTPUT);
        writeRelayPin(pin, false);
    }
    digitalWrite(PUMP_RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
    pinMode(PUMP_RELAY_PIN, OUTPUT);
    writeRelayPin(PUMP_RELAY_PIN, false);
}

void Controller::applyConfig(const Config &cfg) {
    cfg_ = cfg;
}

void Controller::addEvent(const char *src, const char *msg, time_t now) {
    EventRecord &e = events_[event_head_];
    e.t = now;
    copyEventStr(e.src, sizeof(e.src), src);
    copyEventStr(e.msg, sizeof(e.msg), msg);
    event_head_ = (uint8_t)((event_head_ + 1) % EVENT_LOG_SIZE);
    if (event_count_ < EVENT_LOG_SIZE) event_count_++;
}

const EventRecord &Controller::event(uint8_t i) const {
    uint8_t start = (uint8_t)((event_head_ + EVENT_LOG_SIZE - event_count_) % EVENT_LOG_SIZE);
    uint8_t idx = (uint8_t)((start + i) % EVENT_LOG_SIZE);
    return events_[idx];
}

int8_t Controller::recurringAction(uint8_t channel, time_t now) {
    SchedDate today, yesterday;
    schedDate(now, today);
    schedDate(now - 86400, yesterday);
    struct tm tm;
    localtime_r(&now, &tm);
    uint16_t now_min = (uint16_t)(tm.tm_hour * 60 + tm.tm_min);

    bool off = false;
    bool on = false;
    for (uint8_t i = 0; i < cfg_.nrecurring; i++) {
        const RecurringSchedule &rs = cfg_.recurring[i];
        if (!rs.enabled) continue;
        if ((rs.channel_mask & (uint16_t)(1u << channel)) == 0) continue;
        if (!recurringCovers(rs.repeat, rs.days, rs.day_of_month, rs.month,
                             rs.from_min, rs.to_min, today, yesterday, now_min))
            continue;
        if (rs.action == RACT_ON) on = true;
        else if (rs.action == RACT_OFF) off = true;
    }
    if (off) return 0;   // an explicit off schedule wins
    if (on) return 1;
    return -1;
}

bool Controller::relayWanted(uint8_t i, bool power, uint16_t now_min,
                             uint8_t weekday, time_t now,
                             bool &recurring_active) {
    const ChannelConfig &ch = cfg_.channels[i];
    int8_t rec = recurringAction(i, now);
    recurring_active = rec >= 0;
    if (!power) return false;
    if (ch.mode == MODE_ON) return true;
    if (ch.mode == MODE_OFF) return false;
    if (rec == 0) return false;
    if (rec == 1) return true;
    bool sched = ch.enabled &&
                 scheduleActive(ch.days, ch.win, ch.nwin, weekday, now_min);
    bool boost = relay_[i].boost_until > now;
    return sched || boost;
}

bool Controller::pumpWanted(bool power, bool tank_full, bool sump_dry,
                            uint16_t now_min, uint8_t weekday, time_t now) {
    const PumpConfig &p = cfg_.pump;
    if (!power || pump_.fault_latched) return false;
    if (!p.enabled) return false;
    if (tank_full || sump_dry) return false;

    bool want = false;
    if (p.mode == MODE_ON) want = true;
    else if (p.mode == MODE_OFF) want = false;
    else {
        bool sched = scheduleActive(p.days, p.win, p.nwin, weekday, now_min);
        bool boost = pump_.boost_until > now;
        want = sched || boost;
    }

    if (want && !p.resume_after_outage && pump_.need_edge && p.mode == MODE_AUTO)
        want = false;
    return want;
}

void Controller::tick(time_t now, bool power_present, bool tank_full,
                      bool sump_dry, float current_a, float battery_v) {
    bool was_power = power_;
    bool was_tank = pump_.tank_full;
    bool was_sump = pump_.sump_dry;
    bool was_external = pump_.external_run;

    battery_v_ = battery_v;
    if (battery_v_ > 2.0f && battery_v_ < DEFAULT_BATTERY_LOW_V &&
        (last_batt_log_ == 0 || now - last_batt_log_ >= 600)) {
        addEvent("power", "battery low", now);
        last_batt_log_ = now;
    }

    if (power_present && !was_power) {
        restored_at_ = now;
        addEvent("power", "mains restored", now);
    } else if (!power_present && was_power) {
        outage_start_ = now;
        addEvent("power", "mains lost", now);
        if (!cfg_.pump.resume_after_outage) pump_.need_edge = true;
    }
    power_ = power_present;

    if (tank_full != was_tank)
        addEvent("sensor", tank_full ? "tank full" : "tank clear", now);
    if (sump_dry != was_sump)
        addEvent("sensor", sump_dry ? "sump dry" : "sump ok", now);
    pump_.tank_full = tank_full;
    pump_.sump_dry = sump_dry;

    float measured = current_a;
    if (measured < 0) measured = 0;
    pump_.current_a = pump_.current_a * 0.5f + measured * 0.5f;
    if (pump_.current_a < 0) pump_.current_a = 0;

    struct tm tm;
    localtime_r(&now, &tm);
    uint8_t weekday = (uint8_t)((tm.tm_wday + 6) % 7);  // Mon=0
    uint16_t now_min = (uint16_t)(tm.tm_hour * 60 + tm.tm_min);

    // ---- 12 scheduled relays -------------------------------------------------
    for (uint8_t i = 0; i < MAX_CHANNELS; i++) {
        ChannelConfig &ch = cfg_.channels[i];
        RelayRuntime &rt = relay_[i];

        if (rt.boost_until != 0 && now >= rt.boost_until)
            rt.boost_until = 0;
        rt.boost_active = rt.boost_until > now;

        rt.schedule_active =
            ch.enabled &&
            scheduleActive(ch.days, ch.win, ch.nwin, weekday, now_min);

        if (!power_present) {
            if (rt.on || rt.output) {
                rt.on = false;
                rt.output = false;
                rt.pulsing = false;
                rt.last_change = now;
                writeRelayPin(RELAY_PINS[i], false);
            }
            rt.recurring_active = false;
            continue;
        }

        if (ch.mode == MODE_OFF) rt.boost_until = 0;

        bool rec_active = false;
        bool want = relayWanted(i, power_present, now_min, weekday, now, rec_active);
        rt.recurring_active = rec_active;
        if (want != rt.on) {
            setRelayLogical(i, want, now);
            char msg[52];
            snprintf(msg, sizeof(msg), "R%u %s%s%s", (unsigned)(i + 1),
                     want ? "ON" : "OFF",
                     ch.pulse_mode ? " (pulse)" : "",
                     rt.boost_active ? " (boost)" :
                     (ch.mode == MODE_ON ? " (manual)" : " (schedule)"));
            addEvent("relay", msg, now);
        }
    }

    // ---- dedicated pump output ----------------------------------------------
    const PumpConfig &p = cfg_.pump;
    if (pump_.boost_until != 0 && now >= pump_.boost_until)
        pump_.boost_until = 0;
    pump_.boost_active = pump_.boost_until > now;

    pump_.schedule_active =
        p.enabled && scheduleActive(p.days, p.win, p.nwin, weekday, now_min);

    bool want = pumpWanted(power_present, tank_full, sump_dry, now_min, weekday, now);
    pump_.want = want;

    bool flow = pump_.current_a >= 0.5f;

    if (!power_present) {
        if (pump_.on) setPumpOutput(false, now);
        pump_.last_off = now;
        pump_.external_run = false;
    } else if (pump_.fault_latched) {
        if (pump_.on) setPumpOutput(false, now);
        pump_.last_off = now;
    } else {
        // Detect an externally-run pump (selector in HAND, or another starter).
        if (was_external && !flow) {
            pump_.external_run = false;
            pump_.last_off = now;
            addEvent("pump", "external run ended", now);
        } else if (!was_external && flow && !pump_.on) {
            pump_.external_run = true;
            addEvent("pump", "external run detected (HAND?)", now);
        }

        if (want) {
            if (!pump_.on && !pump_.external_run) {
                bool min_ok = (pump_.last_off == 0) ||
                              ((now - pump_.last_off) >= (time_t)p.min_off_sec);
                bool rec_ok = (restored_at_ == 0) ||
                              ((now - restored_at_) >= (time_t)p.recovery_delay_sec);
                bool edge_ok = true;
                if (!p.resume_after_outage && pump_.need_edge && p.mode == MODE_AUTO)
                    edge_ok = false;

                if (!pump_.schedule_active) pump_.need_edge = false;
                if (p.mode == MODE_ON) pump_.need_edge = false;

                if (min_ok && rec_ok && edge_ok) {
                    setPumpOutput(true, now);
                    pump_.run_start = now;
                    addEvent("pump", "started", now);
                }
            }
        } else {
            if (pump_.on) {
                const char *reason = tank_full ? "tank full" :
                                     (sump_dry ? "sump dry" : "schedule");
                char msg[48];
                snprintf(msg, sizeof(msg), "stopped (%s)", reason);
                setPumpOutput(false, now);
                pump_.last_off = now;
                addEvent("pump", msg, now);
            }
        }

        // Running protections
        if (pump_.on) {
            time_t run = now - pump_.run_start;
            if (run > (time_t)p.max_run_min * 60) {
                tripPump("FAULT: max run time", now);
            } else if (run > (time_t)p.dry_run_grace_sec &&
                       pump_.current_a < p.dry_run_current_a) {
                tripPump("FAULT: dry run / no current", now);
            }
        }
    }

    if (!pump_.schedule_active) pump_.need_edge = false;
}

void Controller::tripPump(const char *reason, time_t now) {
    if (pump_.on) setPumpOutput(false, now);
    pump_.on = false;
    pump_.last_off = now;
    pump_.last_change = now;
    pump_.fault_latched = true;
    copyEventStr(pump_.fault, sizeof(pump_.fault), reason);
    addEvent("pump", reason, now);
}

void Controller::setRelayMode(uint8_t index, RunMode mode, uint16_t boost_min,
                              time_t now, uint16_t pulse_ms) {
    if (index >= MAX_CHANNELS) return;
    ChannelConfig &ch = cfg_.channels[index];
    RelayRuntime &rt = relay_[index];

    char msg[52];
    if (boost_min > 0 && mode != MODE_OFF) {
        if (boost_min > cfg_.max_boost_min) boost_min = cfg_.max_boost_min;
        ch.mode = MODE_AUTO;
        rt.boost_until = now + (time_t)boost_min * 60;
        snprintf(msg, sizeof(msg), "R%u boost %umin", (unsigned)(index + 1),
                 (unsigned)boost_min);
    } else {
        ch.mode = mode;
        rt.boost_until = 0;
        if (ch.pulse_mode) {
            if (mode == MODE_ON) {
                uint16_t ms = pulse_ms ? pulse_ms : ch.pulse_on_ms;
                setRelayLogical(index, true, now);
                if (ms != ch.pulse_on_ms) {
                    rt.pulse_until_ms = pulse_clock_ms_ + (unsigned long)ms;
                }
            } else if (mode == MODE_OFF && rt.on) {
                uint16_t ms = pulse_ms ? pulse_ms : ch.pulse_off_ms;
                setRelayLogical(index, false, now);
                if (ms != ch.pulse_off_ms) {
                    rt.pulse_until_ms = pulse_clock_ms_ + (unsigned long)ms;
                }
            } else if (mode == MODE_OFF) {
                rt.on = false;
                rt.output = false;
                rt.pulsing = false;
                writeRelayPin(RELAY_PINS[index], false);
            }
        } else if (mode == MODE_OFF) {
            rt.on = false;
            rt.output = false;
            rt.pulsing = false;
            writeRelayPin(RELAY_PINS[index], false);
        }
        snprintf(msg, sizeof(msg), "R%u mode %s%s", (unsigned)(index + 1),
                 runModeText(mode), ch.pulse_mode ? " (pulse)" : "");
    }
    addEvent("control", msg, now);
}

void Controller::setPumpMode(RunMode mode, uint16_t boost_min, time_t now) {
    PumpConfig &p = cfg_.pump;
    char msg[48];
    if (boost_min > 0 && mode != MODE_OFF) {
        if (boost_min > cfg_.max_boost_min) boost_min = cfg_.max_boost_min;
        p.mode = MODE_AUTO;
        pump_.boost_until = now + (time_t)boost_min * 60;
        snprintf(msg, sizeof(msg), "pump boost %umin", (unsigned)boost_min);
    } else {
        p.mode = mode;
        pump_.boost_until = 0;
        if (mode == MODE_ON) pump_.need_edge = false;
        if (mode == MODE_OFF && pump_.on) {
            setPumpOutput(false, now);
            pump_.last_off = now;
        }
        snprintf(msg, sizeof(msg), "pump mode %s", runModeText(mode));
    }
    addEvent("control", msg, now);
}

void Controller::allOff(time_t now) {
    for (uint8_t i = 0; i < MAX_CHANNELS; i++) {
        cfg_.channels[i].mode = MODE_OFF;
        relay_[i].boost_until = 0;
        relay_[i].on = false;
        relay_[i].output = false;
        relay_[i].pulsing = false;
        writeRelayPin(RELAY_PINS[i], false);
    }
    cfg_.pump.mode = MODE_OFF;
    pump_.boost_until = 0;
    if (pump_.on) {
        setPumpOutput(false, now);
        pump_.last_off = now;
    }
    addEvent("control", "ALL OFF", now);
}

void Controller::allAuto(time_t now) {
    for (uint8_t i = 0; i < MAX_CHANNELS; i++) {
        cfg_.channels[i].mode = MODE_AUTO;
        relay_[i].boost_until = 0;
    }
    cfg_.pump.mode = MODE_AUTO;
    pump_.boost_until = 0;
    pump_.need_edge = false;
    addEvent("control", "ALL AUTO", now);
}

void Controller::resetPumpFault(time_t now) {
    pump_.fault_latched = false;
    pump_.fault[0] = '\0';
    pump_.need_edge = false;
    addEvent("control", "pump fault reset", now);
}

time_t Controller::outageSeconds(time_t now) const {
    if (power_ || outage_start_ == 0) return 0;
    return now - outage_start_;
}

const char *Controller::pumpStateName(time_t now) const {
    if (pump_.fault_latched) return "FAULT";
    if (!power_) return "NO POWER";
    if (pump_.external_run) return "EXTERNAL";
    if (pump_.on) {
        time_t run = now - pump_.run_start;
        if (run <= (time_t)cfg_.pump.dry_run_grace_sec &&
            pump_.current_a < cfg_.pump.dry_run_current_a)
            return "STARTING";
        return "RUNNING";
    }
    if (pump_.want) return "WAITING";
    return "IDLE";
}
