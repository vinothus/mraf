// MRAF Zephyr port - small Arduino compatibility layer.
//
// The controller, model and settings code is shared with the original
// Arduino firmware. Only the tiny surface the controller needs (GPIO, millis)
// and a fixed-size String used for validation messages are provided here.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <zephyr/kernel.h>

#include "mraf_hal.h"

#ifndef LOW
#define LOW 0
#define HIGH 1
#endif
#ifndef INPUT
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#endif
#ifndef PROGMEM
#define PROGMEM
#endif

// Fixed-capacity String replacement. It deliberately avoids the C++ standard
// library so the same source builds for xtensa (no libstdc++) and native_sim.
class String {
public:
    String() { buf_[0] = '\0'; len_ = 0; }
    String(const char *s) { assign(s); }
    String(const String &o) { assign(o.buf_); }
    String &operator=(const char *s) { assign(s); return *this; }
    String &operator=(const String &o) { assign(o.c_str()); return *this; }

    const char *c_str() const { return buf_; }
    unsigned int length() const { return len_; }
    char operator[](size_t i) const { return buf_[i]; }
    bool operator==(const char *s) const { return strcmp(buf_, s) == 0; }
    bool operator!=(const char *s) const { return !(*this == s); }
    operator const char *() const { return buf_; }

    bool concat(const char *s) {
        if (!s) return false;
        size_t add = strlen(s);
        if (len_ + add >= kCap) add = kCap - 1 - len_;
        memcpy(buf_ + len_, s, add);
        len_ = (unsigned int)(len_ + add);
        buf_[len_] = '\0';
        return add > 0;
    }
    bool concat(const String &s) { return concat(s.c_str()); }
    String &operator+=(const char *s) { concat(s); return *this; }
    String &operator+=(const String &s) { concat(s.c_str()); return *this; }

private:
    static const size_t kCap = 192;
    char buf_[kCap];
    unsigned int len_;

    void assign(const char *s) {
        if (!s) s = "";
        size_t n = strlen(s);
        if (n >= kCap) n = kCap - 1;
        memcpy(buf_, s, n);
        buf_[n] = '\0';
        len_ = (unsigned int)n;
    }
};

inline String operator+(String a, const char *b) { a.concat(b); return a; }
inline String operator+(String a, const String &b) { a.concat(b.c_str()); return a; }
inline String operator+(String a, int v) {
    char tmp[16];
    snprintf(tmp, sizeof(tmp), "%d", v);
    a.concat(tmp);
    return a;
}
inline String operator+(String a, unsigned int v) {
    char tmp[16];
    snprintf(tmp, sizeof(tmp), "%u", v);
    a.concat(tmp);
    return a;
}

inline void pinMode(uint8_t pin, uint8_t mode) { mraf_gpio_config(pin, mode); }
inline void digitalWrite(uint8_t pin, uint8_t value) { mraf_gpio_write(pin, value); }
inline int digitalRead(uint8_t pin) { return mraf_gpio_read(pin); }
inline unsigned long millis() { return (unsigned long)k_uptime_get_32(); }
inline void delay(unsigned long ms) { k_msleep((int32_t)ms); }
inline void yield() { k_yield(); }

// Zephyr's minimal libc localtime_r is UTC-only. The controller logic is
// wall-clock local-time based, so route every call through the MRAF POSIX-TZ
// aware implementation.
struct tm *mraf_localtime_r(const time_t *t, struct tm *out);
#define localtime_r(t, out) mraf_localtime_r((t), (out))
