#pragma once

#include "mraf_compat.h"
#include <ArduinoJson.h>
#include "model.h"

// Fill a Config with safe factory defaults.
void settingsDefaults(Config &cfg);

// Load from NVS. Returns false when no valid config existed.
bool settingsLoad(Config &cfg);

// Persist to NVS (includes Wi-Fi/auth secrets).
bool settingsSave(const Config &cfg);

// Serialize for the REST API. Secrets are never included unless
// include_secrets is true (used for NVS only).
void settingsToJson(const Config &cfg, JsonDocument &doc, bool include_secrets);

// Apply a JSON object to cfg. Missing fields keep their current values.
// Empty auth_pass / wifi_password keep the existing secret.
// Returns false with an error message on invalid input.
bool settingsFromJson(JsonDocument &doc, Config &cfg, String &error);

// Time helpers
uint16_t hhmmToMinutes(const char *s);              // 0xFFFF on error
void minutesToHHMM(uint16_t m, char *out, size_t n);
const char *modeName(RunMode m);
RunMode modeFromString(const char *s, bool &ok);
const char *profileName(uint8_t p);
const char *equipmentName(uint8_t e);
const char *repeatName(uint8_t r);
const char *actionName(uint8_t a);
String localTimeString(time_t t);
