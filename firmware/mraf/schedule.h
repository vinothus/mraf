#pragma once

#include <stdint.h>

// Pure scheduling logic. No Arduino dependencies: this file is also compiled
// by the native host test (tests/test_schedule.cpp) with g++.

struct SchedWindow {
    uint16_t on_min;
    uint16_t off_min;
};

inline bool schedBit(uint8_t mask, uint8_t bit) {
    return (mask & (uint8_t)(1u << bit)) != 0;
}

// True when a single window covers now_min on a day whose bit is set.
// Windows with on == off are treated as "never" to avoid ambiguity.
// Windows that cross midnight are handled in scheduleActive().
inline bool windowCovers(uint16_t now_min, uint16_t on_min, uint16_t off_min) {
    if (on_min == off_min) return false;
    if (on_min < off_min) return now_min >= on_min && now_min < off_min;
    return now_min >= on_min;   // evening part; morning part is handled by caller
}

// days: bit0=Monday ... bit6=Sunday
// weekday: 0=Monday ... 6=Sunday
// now_min: minutes since local midnight
inline bool scheduleActive(uint8_t days, const SchedWindow *wins, uint8_t nwin,
                           uint8_t weekday, uint16_t now_min) {
    for (uint8_t i = 0; i < nwin; i++) {
        const uint16_t on = wins[i].on_min;
        const uint16_t off = wins[i].off_min;
        if (on == off) continue;
        if (on < off) {
            if (schedBit(days, weekday) && now_min >= on && now_min < off)
                return true;
        } else {
            // Crosses midnight: evening part belongs to today, morning part
            // belongs to the previous day's schedule bit.
            if (now_min >= on && schedBit(days, weekday)) return true;
            const uint8_t prev = (weekday == 0) ? 6 : (uint8_t)(weekday - 1);
            if (now_min < off && schedBit(days, prev)) return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Day / week / month / year recurring schedules
// ---------------------------------------------------------------------------
struct SchedDate {
    uint16_t year;
    uint8_t month;      // 1..12
    uint8_t day;        // 1..31
    uint8_t weekday;    // 0=Monday ... 6=Sunday
};

// repeat: 0=daily, 1=weekly, 2=monthly, 3=yearly
inline bool recurringDateMatches(uint8_t repeat, uint8_t days, uint8_t dom,
                                 uint8_t month, const SchedDate &d) {
    switch (repeat) {
        case 0: return true;
        case 1: return schedBit(days, d.weekday);
        case 2: return d.day == dom;
        case 3: return d.month == month && d.day == dom;
        default: return false;
    }
}

// Evaluates a recurring window. from_min == to_min means "never". A window
// that crosses midnight uses today's date for the evening part and
// yesterday's date for the early-morning part, so "every Monday 22:00-02:00"
// correctly covers Tuesday 01:00.
inline bool recurringCovers(uint8_t repeat, uint8_t days, uint8_t dom,
                            uint8_t month, uint16_t from_min, uint16_t to_min,
                            const SchedDate &today, const SchedDate &yesterday,
                            uint16_t now_min) {
    if (from_min == to_min) return false;
    if (from_min < to_min) {
        return recurringDateMatches(repeat, days, dom, month, today) &&
               now_min >= from_min && now_min < to_min;
    }
    if (now_min >= from_min &&
        recurringDateMatches(repeat, days, dom, month, today))
        return true;
    if (now_min < to_min &&
        recurringDateMatches(repeat, days, dom, month, yesterday))
        return true;
    return false;
}
