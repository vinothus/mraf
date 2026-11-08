#pragma once

#include <Arduino.h>
#include <time.h>

// Minimal DS3231 driver (also works with a DS1307 for basic time).
// The RTC is optional: if it is absent the ESP32 uses NTP and its internal
// clock, which stays alive on the battery rail.

bool rtcBegin();
bool rtcPresent();
bool rtcReadLocal(time_t &out);
bool rtcWriteLocal(time_t t);
