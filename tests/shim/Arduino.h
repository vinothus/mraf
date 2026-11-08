#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef LOW
#define LOW 0
#endif
#ifndef HIGH
#define HIGH 1
#endif
#ifndef INPUT
#define INPUT 0
#endif
#ifndef OUTPUT
#define OUTPUT 1
#endif
#ifndef INPUT_PULLUP
#define INPUT_PULLUP 2
#endif

extern int g_mraf_pin_state[64];

inline void pinMode(uint8_t, uint8_t) {}

inline void digitalWrite(uint8_t pin, uint8_t value) {
    if (pin < 64) g_mraf_pin_state[pin] = value;
}

inline int digitalRead(uint8_t) { return LOW; }

inline unsigned long millis() { return 0; }
