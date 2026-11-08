// MRAF Farm Controller - build-time configuration and pin map
// Target board: ESP32 DevKit V1 38-pin (ESP32-WROOM-32), Arduino-ESP32 core.
//
// IMPORTANT: this design deliberately uses only off-the-shelf modules.
// There is no custom PCB. All connections are made on DIN-rail terminals.
//
// Relay module: 16-channel, 5 V coils, active-LOW inputs, with a JD-VCC jumper.
//   Remove the JD-VCC jumper.
//   VCC  -> ESP32 3.3 V (logic side)
//   GND  -> ESP32 GND   (logic side)
//   JD-VCC -> 5 V relay-rail SMPS (coil side, mains-derived)
//   coil GND -> 5 V SMPS GND
//   IN1..IN13 -> ESP32 relay pins below
// This keeps the relay coils on the mains-derived 5 V rail so they all drop
// out during a power cut, while the ESP32 stays alive on the battery rail.

#pragma once

#include <stdint.h>

#define FW_VERSION "1.1.0"
#define MAX_CHANNELS 12
#define MAX_WINDOWS 4
#define MAX_RECURRING 8
#define EVENT_LOG_SIZE 48

// ---------------------------------------------------------------------------
// Outputs (all relay module inputs, active LOW)
// ---------------------------------------------------------------------------
// 12 scheduled loads. These GPIOs are safe on the ESP32 (no strapping pins).
static const uint8_t RELAY_PINS[MAX_CHANNELS] = {
    13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27
};

// Dedicated 5 HP 3-phase pump contactor relay.
static const uint8_t PUMP_RELAY_PIN = 33;

// Relay board outputs are active LOW on the common 16-channel module.
static const bool RELAY_ACTIVE_LOW = true;

// ---------------------------------------------------------------------------
// Inputs / sensors (ADC1 only: ADC2 is unusable while Wi-Fi is on)
// ---------------------------------------------------------------------------
// Mains presence: 5 V relay rail -> 22k/15k divider -> GPIO32
static const uint8_t MAINS_SENSE_PIN = 32;
// Float switches. Use external 10k pull-ups to 3.3 V; switch closes to GND.
static const uint8_t TANK_FULL_PIN = 34;
static const uint8_t SUMP_DRY_PIN = 35;
// 12 V battery -> 100k/22k divider -> GPIO36
static const uint8_t BATT_SENSE_PIN = 36;
// ACS712-30A module output -> 2:1 divider -> GPIO39 (see docs)
static const uint8_t PUMP_CURRENT_PIN = 39;

// DS3231 RTC on a second I2C bus (any GPIO is allowed on ESP32).
// GPIO5 is a strapping pin that must be HIGH at boot; the RTC pull-ups make
// that safe. GPIO4 is a normal GPIO.
static const uint8_t RTC_SDA_PIN = 4;
static const uint8_t RTC_SCL_PIN = 5;

// On-board blue LED on most DevKit V1 boards (active HIGH).
static const uint8_t STATUS_LED_PIN = 2;

// ---------------------------------------------------------------------------
// Analog scaling
// ---------------------------------------------------------------------------
// Battery divider: 100k top, 22k bottom -> ratio (100 + 22) / 22.
static const float BATT_DIVIDER_RATIO = (100.0f + 22.0f) / 22.0f;
// ACS712-30A is 66 mV/A. A 2:1 divider is REQUIRED because the 5 V output
// would exceed the 3.3 V ESP32 ADC during motor starting.
static const float ACS_SENSITIVITY_MV_PER_A = 66.0f / 2.0f;
// Mains-sense divider: 22k top, 15k bottom. HIGH = mains present.
static const bool MAINS_SENSE_ACTIVE_HIGH = true;

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------
static const char *DEFAULT_DEVICE_NAME = "MRAF Farm Controller";
static const char *DEFAULT_TIMEZONE = "IST-5:30";   // POSIX TZ for UTC+05:30
static const char *DEFAULT_AUTH_USER = "admin";
static const char *DEFAULT_AUTH_PASS = "farm1234";
static const char *DEFAULT_HOSTNAME = "mraf";
static const char *DEFAULT_AP_PASS = "farm1234";

// Default credentials the controller uses to identify itself to the remote
// server. The server stores a hash and only the matching user/password is
// allowed to open that device's dashboard.
static const char *DEFAULT_REMOTE_USER = "farm";
static const char *DEFAULT_REMOTE_PASS = "farm1234";

static const uint16_t DEFAULT_COAP_PORT = 5683;
static const char *DEFAULT_COAP_PATH = "/mraf";
static const uint16_t DEFAULT_COAP_INTERVAL_SEC = 60;

// Momentary relay pulse used by motor/contactor equipment. A motor starter
// coil is normally driven by a momentary START/STOP push, so the relay is
// energised for a short configurable pulse instead of being held on.
static const uint16_t DEFAULT_PULSE_ON_MS = 600;
static const uint16_t DEFAULT_PULSE_OFF_MS = 600;

static const uint16_t DEFAULT_MAX_BOOST_MIN = 180;
static const uint16_t DEFAULT_MIN_OFF_SEC = 300;      // 5 min anti short-cycle
static const uint16_t DEFAULT_MAX_RUN_MIN = 120;      // 2 h continuous limit
static const uint16_t DEFAULT_RECOVERY_DELAY_SEC = 30;
static const float DEFAULT_DRY_RUN_CURRENT_A = 1.5f;
static const uint16_t DEFAULT_DRY_RUN_GRACE_SEC = 20;

// LiFePO4 4S pack: warn before the flat part of the curve.
static const float DEFAULT_BATTERY_LOW_V = 12.0f;

static const char *NTP_SERVER_1 = "pool.ntp.org";
static const char *NTP_SERVER_2 = "time.google.com";
static const char *NTP_SERVER_3 = "in.pool.ntp.org";
