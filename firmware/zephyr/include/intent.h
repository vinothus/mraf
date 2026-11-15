// Deterministic, offline intent grammar shared by the on-device chat endpoint
// and the MCP "chat" tool. This mirrors the Java IntentParser so a phrase
// behaves identically whether it is spoken to the device or to the server.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#include <ArduinoJson.h>

#include "mraf_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

struct MrafIntent {
    const char *action;     // "control" | "status" | "data" | "help" | "unknown"
    const char *target;     // "relay" | "pump" | "all"
    uint8_t relay_id;       // 1..12 when target == relay
    const char *mode;       // "auto" | "on" | "off"
    int pulse_ms;           // 0 when unused
    int boost_min;          // 0 when unused
    bool understood;
    char reply[160];
};

// Parse a phrase into an intent. Never fails: an unrecognised phrase yields an
// "unknown" intent with a help reply.
void mraf_intent_parse(const char *text, MrafIntent &out);

// Execute a parsed intent against the local controller. Caller holds the state
// lock. Returns true when a control action was applied; "status"/"data"/"help"
// intents need no execution.
bool mraf_intent_apply(const MrafIntent &intent, time_t now, String &error);

#ifdef __cplusplus
}
#endif
