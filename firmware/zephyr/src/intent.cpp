// On-device deterministic intent grammar (see include/intent.h).
#include "intent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "mraf_compat.h"
#include "mraf_state.h"

static const char *HELP_TEXT =
    "Try: 'turn on relay 3', 'relay 5 off', 'pulse relay 2 for 800 ms', "
    "'pump on', 'boost pump 30 min', 'all off', 'all auto', 'status'.";

static bool hasWord(const char *t, const char *w)
{
    size_t wl = strlen(w);
    const char *p = t;
    while ((p = strstr(p, w)) != nullptr) {
        bool left = (p == t) || (p[-1] == ' ');
        bool right = (p[wl] == '\0') || (p[wl] == ' ');
        if (left && right) {
            return true;
        }
        p += wl;
    }
    return false;
}

static bool hasAny(const char *t, const char *const *words, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (hasWord(t, words[i])) {
            return true;
        }
    }
    return false;
}

static int numberFollowedBy(const char *t, const char *unit)
{
    size_t ul = strlen(unit);
    const char *p = t;
    while (*p) {
        if (*p >= '0' && *p <= '9') {
            const char *start = p;
            while (*p >= '0' && *p <= '9') {
                p++;
            }
            const char *q = p;
            while (*q == ' ') {
                q++;
            }
            if (strncmp(q, unit, ul) == 0) {
                return atoi(start);
            }
        } else {
            p++;
        }
    }
    return -1;
}

static int findRelay(const char *t)
{
    static const char *const kw[] = {
        "relay", "channel", "output", "zone", "light", "valve",
        "fan", "motor", "socket", "heater", "door"
    };
    for (size_t i = 0; i < sizeof(kw) / sizeof(kw[0]); i++) {
        const char *p = strstr(t, kw[i]);
        if (!p) {
            continue;
        }
        p += strlen(kw[i]);
        while (*p == ' ' || *p == '#') {
            p++;
        }
        if (*p >= '0' && *p <= '9') {
            return atoi(p);
        }
    }
    const char *h = strchr(t, '#');
    if (h) {
        h++;
        while (*h == ' ') {
            h++;
        }
        if (*h >= '0' && *h <= '9') {
            return atoi(h);
        }
    }
    return -1;
}

static const char *detectMode(const char *t)
{
    static const char *const auto_words[] = {"auto", "automatic", "scheduled"};
    static const char *const off_words[] = {
        "off", "stop", "disable", "disabled", "deactivate", "kill"
    };
    static const char *const on_words[] = {"on", "start", "run", "enable", "enabled", "activate"};
    if (hasAny(t, auto_words, sizeof(auto_words) / sizeof(auto_words[0]))) {
        return "auto";
    }
    if (hasAny(t, off_words, sizeof(off_words) / sizeof(off_words[0]))) {
        return "off";
    }
    if (hasAny(t, on_words, sizeof(on_words) / sizeof(on_words[0]))) {
        return "on";
    }
    return nullptr;
}

void mraf_intent_parse(const char *text, MrafIntent &out)
{
    memset(&out, 0, sizeof(out));
    out.action = "unknown";
    out.target = "";
    out.mode = "";
    out.understood = false;

    char t[160];
    size_t j = 0;
    for (const char *p = text ? text : ""; *p && j < sizeof(t) - 1; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '#' || c == ':' || c == '.') {
            t[j++] = c;
        } else if (j > 0 && t[j - 1] != ' ') {
            t[j++] = ' ';
        }
    }
    while (j > 0 && t[j - 1] == ' ') {
        j--;
    }
    t[j] = '\0';

    if (t[0] == '\0') {
        out.action = "help";
        out.understood = true;
        snprintf(out.reply, sizeof(out.reply), "%s", HELP_TEXT);
        return;
    }
    if (hasWord(t, "help") || hasWord(t, "commands") || strcmp(t, "?") == 0 ||
        strncmp(t, "what can you do", 15) == 0 || strncmp(t, "how do i", 8) == 0) {
        out.action = "help";
        out.understood = true;
        snprintf(out.reply, sizeof(out.reply), "%s", HELP_TEXT);
        return;
    }

    static const char *const status_words[] = {"status", "report", "summary"};
    bool wantsStatus = hasAny(t, status_words, sizeof(status_words) / sizeof(status_words[0]));
    static const char *const data_words[] = {
        "data", "telemetry", "reading", "readings", "sensors", "sensor"
    };
    static const char *const data_verbs[] = {
        "request", "get", "fetch", "read", "send", "give", "now", "poll"
    };
    bool wantsData = strcmp(t, "data") == 0 ||
        (hasAny(t, data_words, sizeof(data_words) / sizeof(data_words[0])) &&
         hasAny(t, data_verbs, sizeof(data_verbs) / sizeof(data_verbs[0])));

    if (wantsStatus) {
        out.action = "status";
        out.understood = true;
        snprintf(out.reply, sizeof(out.reply), "Fetching the latest controller status.");
        return;
    }
    if (wantsData) {
        out.action = "data";
        out.understood = true;
        snprintf(out.reply, sizeof(out.reply),
                 "Requesting a fresh status snapshot from the controller.");
        return;
    }

    static const char *const all_words[] = {
        "all", "everything", "every relay", "every output"
    };
    static const char *const pump_words[] = {"pump", "borewell", "contactor"};
    bool all = hasAny(t, all_words, sizeof(all_words) / sizeof(all_words[0]));
    bool pump = hasAny(t, pump_words, sizeof(pump_words) / sizeof(pump_words[0]));
    int pulseMs = numberFollowedBy(t, "ms");
    if (pulseMs < 0) {
        pulseMs = numberFollowedBy(t, "millis");
    }
    int boostMin = numberFollowedBy(t, "min");

    if (hasWord(t, "boost")) {
        int minutes = boostMin < 0 ? 30 : boostMin;
        out.action = "control";
        out.target = "pump";
        out.mode = "auto";
        out.boost_min = minutes;
        out.understood = true;
        snprintf(out.reply, sizeof(out.reply),
                 "Boosting the pump for %d minutes.", minutes);
        return;
    }

    const char *mode = detectMode(t);

    if (all) {
        if (!mode) {
            snprintf(out.reply, sizeof(out.reply),
                     "Tell me whether to turn everything on, off or to auto.");
            return;
        }
        out.action = "control";
        out.target = "all";
        out.understood = true;
        if (strcmp(mode, "on") == 0) {
            out.mode = "auto";
            snprintf(out.reply, sizeof(out.reply),
                     "Turning all outputs on is not allowed; using auto instead.");
        } else {
            out.mode = mode;
            snprintf(out.reply, sizeof(out.reply), "Setting all outputs to %s.", mode);
        }
        return;
    }

    int relayId = findRelay(t);

    if (pump && relayId < 0) {
        if (!mode) {
            snprintf(out.reply, sizeof(out.reply),
                     "Tell me whether to turn the pump on, off or to auto.");
            return;
        }
        out.action = "control";
        out.target = "pump";
        out.mode = mode;
        out.understood = true;
        snprintf(out.reply, sizeof(out.reply), "Setting the pump to %s.", mode);
        return;
    }

    if (relayId >= 0) {
        if (relayId < 1 || relayId > MAX_CHANNELS) {
            snprintf(out.reply, sizeof(out.reply),
                     "Relay %d does not exist; valid ids are 1..%d.", relayId, MAX_CHANNELS);
            return;
        }
        if (pulseMs >= 0) {
            out.action = "control";
            out.target = "relay";
            out.relay_id = (uint8_t)relayId;
            out.mode = "on";
            out.pulse_ms = pulseMs;
            out.understood = true;
            snprintf(out.reply, sizeof(out.reply),
                     "Pulsing relay %d for %d ms.", relayId, pulseMs);
            return;
        }
        if (!mode) {
            snprintf(out.reply, sizeof(out.reply),
                     "Tell me whether to turn relay %d on, off or to auto.", relayId);
            return;
        }
        out.action = "control";
        out.target = "relay";
        out.relay_id = (uint8_t)relayId;
        out.mode = mode;
        out.understood = true;
        snprintf(out.reply, sizeof(out.reply), "Setting relay %d to %s.", relayId, mode);
        return;
    }

    if (mode) {
        snprintf(out.reply, sizeof(out.reply),
                 "Which output should I switch? Mention a relay number or the pump.");
        return;
    }
    snprintf(out.reply, sizeof(out.reply), "I did not understand that. %s", HELP_TEXT);
}

bool mraf_intent_apply(const MrafIntent &intent, time_t now, String &error)
{
    if (strcmp(intent.action, "control") != 0) {
        return false;
    }
    JsonDocument doc;
    JsonObject cmd = doc.to<JsonObject>();
    cmd["target"] = intent.target;
    cmd["mode"] = intent.mode;
    if (strcmp(intent.target, "relay") == 0) {
        cmd["id"] = intent.relay_id;
    }
    if (intent.pulse_ms > 0) {
        cmd["pulse_ms"] = intent.pulse_ms;
    }
    if (intent.boost_min > 0) {
        cmd["boost_min"] = intent.boost_min;
    }
    return mraf_apply_control(cmd, now, error);
}
