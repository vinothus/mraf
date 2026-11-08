#include "rtc.h"
#include "config.h"

#include <Wire.h>

static const uint8_t RTC_ADDR = 0x68;
static bool s_present = false;

static uint8_t bcd2dec(uint8_t v) {
    return (uint8_t)((v >> 4) * 10 + (v & 0x0F));
}

static uint8_t dec2bcd(uint8_t v) {
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

bool rtcBegin() {
    Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);
    Wire.setClock(100000);
    Wire.beginTransmission(RTC_ADDR);
    Wire.write((uint8_t)0x00);
    s_present = (Wire.endTransmission() == 0);
    return s_present;
}

bool rtcPresent() {
    return s_present;
}

bool rtcReadLocal(time_t &out) {
    if (!s_present) return false;

    Wire.beginTransmission(RTC_ADDR);
    Wire.write((uint8_t)0x00);
    if (Wire.endTransmission() != 0) return false;
    if (Wire.requestFrom((uint8_t)RTC_ADDR, (uint8_t)7) != 7) return false;

    uint8_t sec = Wire.read();
    uint8_t min = Wire.read();
    uint8_t hour = Wire.read();
    Wire.read();                    // day of week (not used)
    uint8_t mday = Wire.read();
    uint8_t mon = Wire.read();
    uint8_t year = Wire.read();

    // Reject an unset/oscillator-stopped RTC.
    if (sec == 0xFF && min == 0xFF && hour == 0xFF) return false;

    uint8_t h;
    if (hour & 0x40) {              // 12-hour mode
        h = bcd2dec(hour & 0x1F);
        if (hour & 0x20) h = (h % 12) + 12;   // PM
        else h = h % 12;
    } else {
        h = bcd2dec(hour & 0x3F);
    }

    struct tm tm = {};
    tm.tm_sec = bcd2dec(sec & 0x7F);
    tm.tm_min = bcd2dec(min & 0x7F);
    tm.tm_hour = h;
    tm.tm_mday = bcd2dec(mday & 0x3F);
    tm.tm_mon = bcd2dec(mon & 0x1F) - 1;
    tm.tm_year = 2000 + bcd2dec(year) - 1900;
    tm.tm_isdst = -1;

    time_t t = mktime(&tm);         // TZ is set to local time before this call
    if (t < 1700000000) return false;   // not set / invalid
    out = t;
    return true;
}

bool rtcWriteLocal(time_t t) {
    if (!s_present) return false;

    struct tm tm;
    localtime_r(&t, &tm);

    Wire.beginTransmission(RTC_ADDR);
    Wire.write((uint8_t)0x00);
    Wire.write(dec2bcd((uint8_t)tm.tm_sec));
    Wire.write(dec2bcd((uint8_t)tm.tm_min));
    Wire.write(dec2bcd((uint8_t)tm.tm_hour));   // force 24-hour mode
    Wire.write(dec2bcd((uint8_t)(tm.tm_wday == 0 ? 7 : tm.tm_wday)));
    Wire.write(dec2bcd((uint8_t)tm.tm_mday));
    Wire.write(dec2bcd((uint8_t)(tm.tm_mon + 1)));
    Wire.write(dec2bcd((uint8_t)(tm.tm_year + 1900 - 2000)));
    if (Wire.endTransmission() != 0) return false;

    // Clear the oscillator-stop flag in the status register (0x0F, bit 7).
    Wire.beginTransmission(RTC_ADDR);
    Wire.write((uint8_t)0x0F);
    if (Wire.endTransmission() != 0) return false;
    if (Wire.requestFrom((uint8_t)RTC_ADDR, (uint8_t)1) != 1) return false;
    uint8_t status = Wire.read();
    status &= (uint8_t)~0x80;
    Wire.beginTransmission(RTC_ADDR);
    Wire.write((uint8_t)0x0F);
    Wire.write(status);
    return Wire.endTransmission() == 0;
}
