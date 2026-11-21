#pragma once

#include "mraf_compat.h"
#include "model.h"

class Controller {
public:
    void begin(const Config &cfg);
    void applyConfig(const Config &cfg);

    Config &config() { return cfg_; }
    const Config &config() const { return cfg_; }
    uint8_t channelCount() const {
        uint8_t n = cfg_.relay_count ? cfg_.relay_count : MAX_CHANNELS;
        return n > MAX_CHANNELS ? MAX_CHANNELS : n;
    }

    // Called about once per second. All timers use wall-clock epoch seconds so
    // the same logic works in the accelerated simulator.
    void tick(time_t now, bool power_present, bool tank_full, bool sump_dry,
              float current_a, float battery_v);

    // Millisecond clock for momentary contactor pulses. Call every loop with
    // millis(); the native test calls it with a synthetic value.
    void setPulseClock(unsigned long ms) { pulse_clock_ms_ = ms; }
    void servicePulses();

    void setRelayMode(uint8_t index, RunMode mode, uint16_t boost_min, time_t now,
                      uint16_t pulse_ms = 0);
    void setPumpMode(RunMode mode, uint16_t boost_min, time_t now);
    void allOff(time_t now);
    void allAuto(time_t now);
    void resetPumpFault(time_t now);

    const RelayRuntime &relay(uint8_t index) const { return relay_[index]; }
    const PumpRuntime &pump() const { return pump_; }
    bool powerPresent() const { return power_; }
    float batteryVoltage() const { return battery_v_; }
    time_t outageSeconds(time_t now) const;
    const char *pumpStateName(time_t now) const;

    uint8_t eventCount() const { return event_count_; }
    const EventRecord &event(uint8_t i) const;
    void addEvent(const char *src, const char *msg, time_t now);

private:
    Config cfg_;
    RelayRuntime relay_[MAX_CHANNELS];
    PumpRuntime pump_;
    EventRecord events_[EVENT_LOG_SIZE];
    uint8_t event_head_ = 0;
    uint8_t event_count_ = 0;

    bool power_ = true;
    time_t outage_start_ = 0;
    time_t restored_at_ = 0;
    float battery_v_ = 0.0f;
    time_t last_batt_log_ = 0;
    unsigned long pulse_clock_ms_ = 0;

    uint8_t relayPin(uint8_t i) const { return cfg_.relay_pins[i] ? cfg_.relay_pins[i] : RELAY_PINS[i]; }
    uint8_t relayOffPin(uint8_t i) const {
        uint8_t p = cfg_.relay_off_pins[i] ? cfg_.relay_off_pins[i] : relayPin(i);
        return p ? p : RELAY_PINS[i];
    }
    uint8_t pumpPin() const { return cfg_.pump_pin ? cfg_.pump_pin : PUMP_RELAY_PIN; }
    void writeRelayPin(uint8_t pin, bool on) const;
    void releaseRelayPins(uint8_t i) const;
    void setRelayLogical(uint8_t i, bool on, time_t now);
    void setRelayPhysical(uint8_t i, bool on);
    void startPulse(uint8_t i, uint16_t ms, bool on);
    void setPumpOutput(bool on, time_t now);
    void tripPump(const char *reason, time_t now);
    bool relayWanted(uint8_t i, bool power, uint16_t now_min, uint8_t weekday,
                     time_t now, bool &recurring_active);
    bool pumpWanted(bool power, bool tank_full, bool sump_dry,
                    uint16_t now_min, uint8_t weekday, time_t now);
    int8_t recurringAction(uint8_t channel, time_t now);
};
