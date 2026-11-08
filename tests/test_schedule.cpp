// Native host test for the pure scheduling logic used by the ESP32 firmware.
// Build: g++ -std=c++11 -O2 -Wall -Wextra tests/test_schedule.cpp -o /tmp/test_schedule
#include <cstdio>
#include <cstdlib>
#include "../firmware/mraf/schedule.h"

static int failures = 0;

static void check(bool cond, const char *what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        failures++;
    }
}

static bool active(uint8_t days, uint16_t on, uint16_t off,
                   uint8_t weekday, uint16_t now) {
    SchedWindow w{on, off};
    return scheduleActive(days, &w, 1, weekday, now);
}

int main() {
    const uint16_t t0600 = 6 * 60;
    const uint16_t t0630 = 6 * 60 + 30;
    const uint16_t t0700 = 7 * 60;
    const uint16_t t1800 = 18 * 60;
    const uint16_t t1830 = 18 * 60 + 30;
    const uint16_t t1200 = 12 * 60;
    const uint16_t t2000 = 20 * 60;
    const uint16_t t2200 = 22 * 60;
    const uint16_t t0500 = 5 * 60;
    const uint16_t t0530 = 5 * 60 + 30;
    const uint16_t t2330 = 23 * 60 + 30;

    // Simple same-day window, every day (bit0..bit6 = 127).
    check(active(127, t0600, t0630, 0, t0600), "start is inclusive");
    check(active(127, t0600, t0630, 0, t0600 + 1), "inside window");
    check(!active(127, t0600, t0630, 0, t0630), "end is exclusive");
    check(!active(127, t0600, t0630, 0, t0530), "before window");

    // Day mask: Monday/Wednesday/Friday = bits 0,2,4 = 21.
    check(active(21, t0600, t0630, 0, t0600), "Monday enabled");
    check(!active(21, t0600, t0630, 1, t0600), "Tuesday disabled");
    check(active(21, t0600, t0630, 2, t0600), "Wednesday enabled");
    check(!active(21, t0600, t0630, 6, t0600), "Sunday disabled");

    // Cross-midnight window 18:30 -> 05:30 every day.
    check(active(127, t1830, t0530, 0, t1830), "cross midnight evening start");
    check(active(127, t1830, t0530, 0, t2330), "cross midnight evening inside");
    check(active(127, t1830, t0530, 1, t0500), "cross midnight early morning");
    check(!active(127, t1830, t0530, 1, t0530), "cross midnight morning end");
    check(!active(127, t1830, t0530, 1, t1200), "cross midnight midday off");

    // Cross-midnight morning belongs to the previous day's mask.
    check(active(1, t1830, t0530, 0, t2000), "Monday evening enabled");
    check(active(1, t1830, t0530, 1, t0500), "Tuesday 05:00 uses Monday bit");
    check(!active(1, t1830, t0530, 0, t0500), "Monday 05:00 uses Sunday bit");
    check(!active(1, t1830, t0530, 2, t0500), "Wednesday 05:00 uses Tuesday bit");

    // on == off is treated as never.
    check(!active(127, t0600, t0600, 0, t0600), "zero length window never active");

    // Multiple windows in one channel.
    SchedWindow wins[2] = {{t0600, t0630}, {t1800, t1830}};
    check(scheduleActive(127, wins, 2, 0, t0600), "first window");
    check(scheduleActive(127, wins, 2, 0, t1800), "second window");
    check(!scheduleActive(127, wins, 2, 0, t1200), "between windows");

    // Monday 00:00 and Sunday wrap edges.
    check(active(127, t2200, t0500, 0, 0), "Monday midnight inside");
    check(active(127, t2200, t0500, 1, 0), "Tuesday midnight inside");
    check(active(127, t0600, t0700, 6, t0600), "Sunday enabled");

    // Recurring date matching.
    const SchedDate mon = {2026, 11, 2, 0};
    const SchedDate tue = {2026, 11, 3, 1};
    const SchedDate nov15 = {2026, 11, 15, 6};
    const SchedDate nov16 = {2026, 11, 16, 0};
    check(recurringDateMatches(0, 0, 1, 1, tue), "daily matches any date");
    check(recurringDateMatches(1, 1, 1, 1, mon), "weekly Monday matches");
    check(!recurringDateMatches(1, 1, 1, 1, tue), "weekly Monday rejects Tuesday");
    check(recurringDateMatches(2, 0, 15, 1, nov15), "monthly day matches");
    check(!recurringDateMatches(2, 0, 15, 1, nov16), "monthly day rejects");
    check(recurringDateMatches(3, 0, 8, 11, {2026, 11, 8, 6}), "yearly matches");
    check(!recurringDateMatches(3, 0, 8, 11, {2026, 12, 8, 1}), "yearly rejects month");
    check(!recurringDateMatches(9, 0, 8, 11, tue), "unknown repeat never matches");

    // Recurring windows, including cross-midnight and previous-day matching.
    check(recurringCovers(0, 0, 1, 1, t0600, t0630, tue, mon, t0600),
          "daily recurring inside window");
    check(!recurringCovers(0, 0, 1, 1, t0600, t0630, tue, mon, t0630),
          "daily recurring end exclusive");
    check(recurringCovers(1, 1, 1, 1, t2200, 120, tue, mon, 60),
          "weekly Monday 22:00-02:00 covers Tuesday 01:00");
    check(!recurringCovers(1, 1, 1, 1, t2200, 120, tue, tue, 60),
          "cross-midnight uses previous day mask");
    check(recurringCovers(1, 1, 1, 1, t2200, 120, mon, mon, t2330),
          "cross-midnight Monday evening");
    check(!recurringCovers(1, 1, 1, 1, t2200, 120, tue, mon, 120),
          "cross-midnight morning end exclusive");
    check(!recurringCovers(0, 0, 1, 1, t0600, t0600, tue, mon, t0600),
          "zero-length recurring never active");

    if (failures) {
        std::printf("%d scheduling test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("all scheduling tests passed\n");
    return EXIT_SUCCESS;
}
