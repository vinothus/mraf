// HTTP dashboard and REST API, served by the Zephyr HTTP server.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <zephyr/kernel.h>
#include <zephyr/net/http/method.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/http/status.h>
#include <zephyr/sys/base64.h>
#include <zephyr/sys/printk.h>

#include "api.h"
#include "config.h"
#include "intent.h"
#include "mcp.h"
#include "mraf_compat.h"
#include "mraf_platform.h"
#include "mraf_state.h"
#include "remote.h"
#include "settings.h"
#include "webui.h"
#include "webrtc.h"

enum RouteId {
    R_ROOT,
    R_HEALTH,
    R_STATUS,
    R_SELFTEST,
    R_CONFIG,
    R_CONTROL,
    R_PUMP_RESET,
    R_TIME,
    R_EVENTS,
    R_SCAN,
    R_REBOOT,
    R_WIREGUARD,
    R_CHAT,
    R_WEBRTC,
    R_MCP,
#if defined(CONFIG_MRAF_SIMULATOR)
    R_SIM_POWER,
    R_SIM_SENSORS,
    R_SIM_CURRENT,
    R_SIM_SPEED,
    R_SIM_STEP,
    R_SIM_RESET,
#endif
    R_NOT_FOUND,
    R_COUNT
};

struct Route;

typedef int (*RouteFn)(struct Route &route, const struct http_request_ctx *request,
                       struct http_response_ctx *response);

struct Route {
    RouteFn fn;
    char *buf;
    size_t cap;
    size_t len;
    bool no_auth;
    bool overflow;
    bool headers_seen;
    bool auth_ok;
};

static Route g_routes[R_COUNT];
static uint16_t g_http_port = CONFIG_MRAF_HTTP_PORT;
static char g_resp[CONFIG_MRAF_HTTP_RESP_MAX];

static char g_buf_config[CONFIG_MRAF_HTTP_REQ_MAX];
static char g_buf_control[1024];
static char g_buf_small[256];
#if defined(CONFIG_MRAF_SIMULATOR)
static char g_buf_sim[512];
static char g_buf_sim_speed[256];
static char g_buf_sim_step[256];
#endif

static const struct http_header g_hdr_json[] = {
    {"Content-Type", "application/json"},
    {"Cache-Control", "no-store"},
};
static const struct http_header g_hdr_html[] = {
    {"Content-Type", "text/html; charset=utf-8"},
    {"Cache-Control", "no-store"},
};
static const struct http_header g_hdr_401[] = {
    {"Content-Type", "application/json"},
    {"Cache-Control", "no-store"},
    {"WWW-Authenticate", "Basic realm=\"MRAF\""},
};

static void sendJson(JsonDocument &doc, int status, struct http_response_ctx *response)
{
    static const char oom_fallback[] =
        "{\"ok\":false,\"error\":\"device out of memory, retry or reboot\"}";
    static const char large_fallback[] =
        "{\"ok\":false,\"error\":\"response too large, retry or reduce events\"}";
    const char *fallback = nullptr;
    size_t fallback_len = 0;
    size_t needed = measureJson(doc);
    size_t n = 0;

    if (needed > 2 && needed < sizeof(g_resp)) {
        n = serializeJson(doc, g_resp, sizeof(g_resp));
    }
    if (n != needed) {
        fallback = needed > 2 ? large_fallback : oom_fallback;
        fallback_len = needed > 2 ? sizeof(large_fallback) - 1 : sizeof(oom_fallback) - 1;
    }

    response->status = (enum http_status)status;
    response->headers = status == 401 ? g_hdr_401 : g_hdr_json;
    response->header_count = status == 401 ? 3 : 2;
    response->body = fallback ? (const uint8_t *)fallback : (const uint8_t *)g_resp;
    response->body_len = fallback ? fallback_len : n;
    response->final_chunk = true;
}

static void sendError(int status, const char *msg, struct http_response_ctx *response)
{
    JsonDocument d;
    d["ok"] = false;
    d["error"] = msg ? msg : "error";
    sendJson(d, status, response);
}

static void copyField(char *dst, size_t n, const char *src)
{
    if (!dst || n == 0) {
        return;
    }
    if (!src) {
        src = "";
    }
    strncpy(dst, src, n - 1);
    dst[n - 1] = '\0';
}

// Turn an ArduinoJson deserialization failure into something an operator can
// act on. "NoMemory" on the ESP32 usually means the system heap is exhausted
// by the Wi-Fi/peer stacks, not that the request was malformed.
static const char *jsonErrText(DeserializationError err)
{
    switch (err.code()) {
    case DeserializationError::NoMemory:
        return "out of memory while parsing request";
    case DeserializationError::EmptyInput:
        return "empty request body";
    case DeserializationError::IncompleteInput:
        return "truncated request body";
    default:
        return err.c_str();
    }
}

static bool headerIs(const char *name, const char *want)
{
    if (!name) {
        return false;
    }
    size_t n = strlen(want);
    return strncasecmp(name, want, n) == 0 && name[n] == '\0';
}

static bool evalAuth(const struct http_request_ctx *request, Route &route)
{
    (void)route;
    if (!request || !request->headers) {
        return false;
    }
    const char *auth = nullptr;
    for (size_t i = 0; i < request->header_count; i++) {
        if (headerIs(request->headers[i].name, "Authorization")) {
            auth = request->headers[i].value;
            break;
        }
    }
    if (!auth || strncasecmp(auth, "Basic ", 6) != 0) {
        return false;
    }
    uint8_t decoded[160];
    size_t olen = 0;
    const char *b64 = auth + 6;
    if (base64_decode(decoded, sizeof(decoded) - 1, &olen,
                      (const uint8_t *)b64, strlen(b64)) < 0) {
        return false;
    }
    decoded[olen] = '\0';

    char expected[96];
    k_mutex_lock(&g_state_lock, K_FOREVER);
    snprintf(expected, sizeof(expected), "%s:%s", g_cfg.auth_user, g_cfg.auth_pass);
    k_mutex_unlock(&g_state_lock);
    return strcmp((const char *)decoded, expected) == 0;
}

static void appendBody(Route &route, const struct http_request_ctx *request)
{
    if (!request || !request->data || request->data_len == 0) {
        return;
    }
    size_t room = route.cap - 1;
    if (route.len + request->data_len > room) {
        route.overflow = true;
        return;
    }
    memcpy(route.buf + route.len, request->data, request->data_len);
    route.len += request->data_len;
}

static int h_config_get(Route &, const struct http_request_ctx *,
                        struct http_response_ctx *response);
static int h_config_post(Route &, const struct http_request_ctx *,
                         struct http_response_ctx *response);
static int h_wireguard_get(Route &, const struct http_request_ctx *,
                           struct http_response_ctx *response);
static int h_wireguard_post(Route &, const struct http_request_ctx *,
                            struct http_response_ctx *response);

static int route_cb(struct http_client_ctx *client, enum http_transaction_status status,
                    const struct http_request_ctx *request,
                    struct http_response_ctx *response, void *user_data)
{
    Route &route = *(Route *)user_data;

    if (status == HTTP_SERVER_TRANSACTION_ABORTED) {
        route.len = 0;
        route.overflow = false;
        route.headers_seen = false;
        return 0;
    }

    if (status == HTTP_SERVER_REQUEST_DATA_MORE) {
        if (!route.headers_seen && request && request->headers) {
            route.auth_ok = route.no_auth ? true : evalAuth(request, route);
            route.headers_seen = true;
        }
        appendBody(route, request);
        return 0;
    }

    if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
        if (!route.headers_seen) {
            route.auth_ok = route.no_auth ? true : evalAuth(request, route);
            route.headers_seen = true;
        }
        appendBody(route, request);
        route.buf[route.len] = '\0';
        printk("MRAF: HTTP method=%d url=%s auth=%d\n", (int)client->method,
               client->url_buffer, route.auth_ok ? 1 : 0);

        if (!route.auth_ok) {
            sendError(401, "authentication required", response);
        } else if (route.overflow) {
            sendError(413, "request body too large", response);
        } else if (&route == &g_routes[R_CONFIG]) {
            if (client->method == HTTP_GET) {
                h_config_get(route, request, response);
            } else if (client->method == HTTP_POST) {
                h_config_post(route, request, response);
            } else {
                sendError(405, "method not allowed", response);
            }
        } else if (&route == &g_routes[R_WIREGUARD]) {
            if (client->method == HTTP_GET) {
                h_wireguard_get(route, request, response);
            } else if (client->method == HTTP_POST) {
                h_wireguard_post(route, request, response);
            } else {
                sendError(405, "method not allowed", response);
            }
        } else if (route.fn) {
            route.fn(route, request, response);
        }
        route.len = 0;
        route.overflow = false;
        return 0;
    }

    if (status == HTTP_SERVER_TRANSACTION_COMPLETE) {
        route.len = 0;
        route.overflow = false;
        route.headers_seen = false;
        route.auth_ok = false;
    }
    return 0;
}

static bool selfTestSafePin(uint8_t pin)
{
    static const uint8_t pins[] = {2,4,5,12,13,14,15,16,17,18,19,21,22,23,
                                   25,26,27,32,33};
    for (size_t i = 0; i < sizeof(pins); i++) {
        if (pins[i] == pin) return true;
    }
    return false;
}

static uint8_t selfTestChannelCount(const Config &cfg)
{
    uint8_t n = cfg.relay_count ? cfg.relay_count : MAX_CHANNELS;
    return n > MAX_CHANNELS ? MAX_CHANNELS : n;
}

static uint8_t selfTestRelayPin(const Config &cfg, uint8_t i)
{
    return cfg.relay_pins[i] ? cfg.relay_pins[i] : RELAY_PINS[i];
}

static uint8_t selfTestRelayOffPin(const Config &cfg, uint8_t i)
{
    uint8_t pin = cfg.relay_off_pins[i] ? cfg.relay_off_pins[i] : selfTestRelayPin(cfg, i);
    return pin ? pin : RELAY_PINS[i];
}

static void selfTestWritePin(uint8_t pin, bool on)
{
    digitalWrite(pin, RELAY_ACTIVE_LOW ? (on ? LOW : HIGH) : (on ? HIGH : LOW));
}

static bool selfTestPinEnergized(uint8_t pin)
{
    int raw = digitalRead(pin);
    return RELAY_ACTIVE_LOW ? raw == LOW : raw == HIGH;
}

static void addSelfTestCheck(JsonArray checks, const char *name, bool ok,
                             const char *detail, const char *severity = "fail")
{
    JsonObject check = checks.add<JsonObject>();
    check["name"] = name;
    check["status"] = ok ? "pass" : severity;
    check["detail"] = detail;
}

// --- handlers -------------------------------------------------------------

static int h_root(Route &, const struct http_request_ctx *, struct http_response_ctx *response)
{
    response->status = HTTP_200_OK;
    response->headers = g_hdr_html;
    response->header_count = 2;
    response->body = (const uint8_t *)INDEX_HTML;
    response->body_len = strlen(INDEX_HTML);
    response->final_chunk = true;
    return 0;
}

static int h_health(Route &, const struct http_request_ctx *, struct http_response_ctx *response)
{
    JsonDocument d;
    d["ok"] = true;
    d["device"] = "mraf";
    d["fw"] = FW_VERSION;
    d["simulator"] = mraf_platform_is_simulator();
    sendJson(d, 200, response);
    return 0;
}

// The browser reads its peer settings from the device (not the server): the
// control-plane URL, the STUN URL and the shared signalling token. This is the
// same document the simulator's webrtc_config() returns.
static int h_webrtc(Route &, const struct http_request_ctx *,
                    struct http_response_ctx *response)
{
    JsonDocument d;
    k_mutex_lock(&g_state_lock, K_FOREVER);
    d["enabled"] = g_cfg.webrtc.enabled;
    d["server"] = g_cfg.webrtc.server;
    d["stun"] = g_cfg.webrtc.stun;
    d["turn"] = g_cfg.webrtc.turn;
    d["turn_user"] = g_cfg.webrtc.turn_user;
    d["turn_pass"] = g_cfg.webrtc.turn_pass;
    d["poll_ms"] = g_cfg.webrtc.poll_ms;
    d["device_id"] = g_cfg.device_id;
    d["token"] = g_cfg.coap_token;
    k_mutex_unlock(&g_state_lock);
    d["transport"] = "datachannel";

    struct mraf_ice_info ice;
    mraf_platform_webrtc_ice_info(&ice);
    JsonObject iceOut = d["ice"].to<JsonObject>();
    iceOut["stun_ok"] = ice.stun_ok;
    iceOut["srflx"] = ice.srflx_addr;
    iceOut["relay"] = ice.relay_addr;
    iceOut["relay_allocated"] = ice.turn_allocated;
    iceOut["path"] = ice.relay_path ? "relay" : "";
    if (ice.turn_error[0]) {
        iceOut["turn_error"] = ice.turn_error;
    }
    sendJson(d, 200, response);
    return 0;
}

static void wireguardToJson(JsonDocument &d)
{
    d["enabled"] = g_cfg.wireguard.enabled;
    d["configured"] = g_cfg.wireguard.configured;
    d["active"] = mraf_platform_wireguard_active();
    d["interface"] = g_cfg.wireguard.interface;
    d["address"] = g_cfg.wireguard.address;
    d["endpoint"] = g_cfg.wireguard.peer_endpoint;
    d["allowed_ips"] = g_cfg.wireguard.peer_allowed_ips;
    d["persistent_keepalive"] = g_cfg.wireguard.persistent_keepalive;
}

static int h_wireguard_get(Route &, const struct http_request_ctx *,
                           struct http_response_ctx *response)
{
    JsonDocument out;
    k_mutex_lock(&g_state_lock, K_FOREVER);
    wireguardToJson(out);
    k_mutex_unlock(&g_state_lock);
    sendJson(out, 200, response);
    return 0;
}

static int h_wireguard_post(Route &route, const struct http_request_ctx *,
                            struct http_response_ctx *response)
{
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, route.buf, route.len);
    if (err) {
        sendError(400, err.c_str(), response);
        return 0;
    }

    JsonDocument out;
    k_mutex_lock(&g_state_lock, K_FOREVER);
    WireGuardConfig &w = g_cfg.wireguard;
    copyField(w.interface, sizeof(w.interface),
              doc["interface"] | DEFAULT_WG_INTERFACE);
    copyField(w.address, sizeof(w.address), doc["address"] | "");
    copyField(w.private_key, sizeof(w.private_key), doc["private_key"] | "");
    copyField(w.dns, sizeof(w.dns), doc["dns"] | "");
    JsonObject peer = doc["peer"].as<JsonObject>();
    if (peer) {
        copyField(w.peer_public_key, sizeof(w.peer_public_key),
                  peer["public_key"] | "");
        copyField(w.peer_endpoint, sizeof(w.peer_endpoint),
                  peer["endpoint"] | "");
        copyField(w.peer_allowed_ips, sizeof(w.peer_allowed_ips),
                  peer["allowed_ips"] | "");
        copyField(w.peer_preshared_key, sizeof(w.peer_preshared_key),
                  peer["preshared_key"] | "");
        int keepalive = peer["persistent_keepalive"] | DEFAULT_WG_KEEPALIVE;
        w.persistent_keepalive = (uint16_t)(keepalive < 0 ? 0 : keepalive);
    }
    w.enabled = true;
    w.configured = w.address[0] && w.private_key[0] && w.peer_public_key[0];

    bool applied = false;
    if (w.configured) {
        mraf_platform_wireguard_apply(w);
        settingsSave(g_cfg);
        g_ctrl.addEvent("remote", "WireGuard config applied", mraf_now());
        applied = true;
    }
    wireguardToJson(out);
    k_mutex_unlock(&g_state_lock);

    out["ok"] = applied;
    out["applied"] = applied;
    if (!applied) {
        out["error"] = "incomplete WireGuard config";
    }
    sendJson(out, applied ? 200 : 400, response);
    return 0;
}

static int h_status(Route &, const struct http_request_ctx *, struct http_response_ctx *response)
{
    JsonDocument d;
    k_mutex_lock(&g_state_lock, K_FOREVER);
    mraf_build_status(d);
    k_mutex_unlock(&g_state_lock);
    sendJson(d, 200, response);
    return 0;
}

static int h_selftest(Route &, const struct http_request_ctx *, struct http_response_ctx *response)
{
    JsonDocument status;
    Config cfg;
    k_mutex_lock(&g_state_lock, K_FOREVER);
    mraf_build_status(status);
    cfg = g_cfg;
    k_mutex_unlock(&g_state_lock);

    JsonDocument doc;
    JsonArray checks = doc["checks"].to<JsonArray>();
    char detail[192];
    char name[48];

    JsonObject wifi = status["wifi"].as<JsonObject>();
    JsonObject power = status["power"].as<JsonObject>();
    JsonObject sensors = status["sensors"].as<JsonObject>();
    JsonObject pump = status["pump"].as<JsonObject>();

    snprintf(detail, sizeof(detail), "%s %s responsive", mraf_platform_name(),
             (const char *)(status["fw"] | FW_VERSION));
    addSelfTestCheck(checks, "ESP32 controller", true, detail);

    bool timeValid = status["time_valid"] | false;
    snprintf(detail, sizeof(detail), "Device time %s · %s",
             (const char *)(status["now_local"] | ""),
             timeValid ? "valid" : "unsynchronised");
    addSelfTestCheck(checks, "RTC clock", timeValid, detail);

    float batteryV = power["battery_v"] | 0.0f;
    bool mains = power["present"] | false;
    bool assumed = power["assumed"] | false;
    int batteryWhole = (int)batteryV;
    int batteryFrac = (int)((batteryV - batteryWhole) * 100.0f + 0.5f);
    snprintf(detail, sizeof(detail), "Mains %s%s · battery-backed %d.%02d V",
             mains ? "present" : "outage", assumed ? " (assumed)" : "",
             batteryWhole, batteryFrac);
    addSelfTestCheck(checks, "Power rail", mains, detail, "warn");
    addSelfTestCheck(checks, "Battery ADC", batteryV >= 11.0f, detail);

    bool wifiConnected = wifi["connected"] | false;
    snprintf(detail, sizeof(detail), "%s · %s · %d dBm",
             (const char *)(wifi["ssid"] | ""), (const char *)(wifi["ip"] | ""),
             (int)(wifi["rssi"] | 0));
    addSelfTestCheck(checks, "Wi-Fi radio", wifiConnected, detail);

    uint8_t channels = selfTestChannelCount(cfg);
    uint8_t used[2 * MAX_CHANNELS + 1];
    size_t usedCount = 0;
    bool pinsOk = true;
    for (uint8_t i = 0; i < channels; i++) {
        uint8_t pins[2] = {selfTestRelayPin(cfg, i), selfTestRelayOffPin(cfg, i)};
        int count = pins[1] == pins[0] ? 1 : 2;
        for (int n = 0; n < count; n++) {
            uint8_t pin = pins[n];
            if (!selfTestSafePin(pin)) pinsOk = false;
            for (size_t j = 0; j < usedCount; j++) {
                if (used[j] == pin) pinsOk = false;
            }
            if (usedCount < sizeof(used)) used[usedCount++] = pin;
        }
    }
    uint8_t pumpPin = cfg.pump_pin;
    uint8_t pumpOffPin = cfg.pump_off_pin;
    if (pumpPin) {
        if (!selfTestSafePin(pumpPin) || !pumpOffPin || pumpOffPin == pumpPin ||
            !selfTestSafePin(pumpOffPin)) pinsOk = false;
        for (size_t j = 0; j < usedCount; j++) {
            if (used[j] == pumpPin || used[j] == pumpOffPin) pinsOk = false;
        }
    }
    snprintf(detail, sizeof(detail), "%u relays%s on unique safe GPIOs",
             (unsigned)channels, pumpPin ? " + pump START/STOP" : " (no pump starter)");
    addSelfTestCheck(checks, "GPIO driver map", pinsOk, detail);

    k_mutex_lock(&g_state_lock, K_FOREVER);
    for (uint8_t i = 0; i < channels; i++) {
        uint8_t pins[2] = {selfTestRelayPin(cfg, i), selfTestRelayOffPin(cfg, i)};
        int count = pins[1] == pins[0] ? 1 : 2;
        bool ok = true;
        char labels[96] = "";
        for (int n = 0; n < count; n++) {
            uint8_t pin = pins[n];
            if (!selfTestSafePin(pin)) {
                ok = false;
                continue;
            }
            int saved = digitalRead(pin);
            selfTestWritePin(pin, true);
            k_msleep(20);
            bool energized = selfTestPinEnergized(pin);
            selfTestWritePin(pin, false);
            k_msleep(5);
            bool released = !selfTestPinEnergized(pin);
            digitalWrite(pin, saved);
            ok = ok && energized && released;
            char label[24];
            snprintf(label, sizeof(label), "%s GPIO %u", n == 0 ? "START" : "STOP",
                     (unsigned)pin);
            if (labels[0]) strncat(labels, ", ", sizeof(labels) - strlen(labels) - 1);
            strncat(labels, label, sizeof(labels) - strlen(labels) - 1);
        }
        snprintf(detail, sizeof(detail), "%s · coil %s", labels,
                 ok ? "pulse/release OK" : "driver readback mismatch");
        snprintf(name, sizeof(name), "Relay %u driver", (unsigned)(i + 1));
        addSelfTestCheck(checks, name, ok, detail);
    }

    bool pumpOk = pinsOk;
    char pumpLabels[64] = "";
    if (pumpPin) {
        uint8_t pumpPins[2] = {pumpPin, pumpOffPin};
        for (int n = 0; n < 2; n++) {
            uint8_t pin = pumpPins[n];
            if (!selfTestSafePin(pin)) { pumpOk = false; continue; }
            int saved = digitalRead(pin);
            selfTestWritePin(pin, true);
            k_msleep(20);
            bool energized = selfTestPinEnergized(pin);
            selfTestWritePin(pin, false);
            k_msleep(5);
            bool released = !selfTestPinEnergized(pin);
            digitalWrite(pin, saved);
            pumpOk = pumpOk && energized && released;
            char label[24];
            snprintf(label, sizeof(label), "%s GPIO %u", n == 0 ? "START" : "STOP",
                     (unsigned)pin);
            if (pumpLabels[0]) strncat(pumpLabels, ", ", sizeof(pumpLabels) - strlen(pumpLabels) - 1);
            strncat(pumpLabels, label, sizeof(pumpLabels) - strlen(pumpLabels) - 1);
        }
    }
    k_mutex_unlock(&g_state_lock);
    const char *pumpFault = pump["fault"] | "";
    if (!pumpFault[0]) pumpFault = "none";
    if (pumpPin) {
        snprintf(detail, sizeof(detail), "%s · coil pulse/release OK · state %s · fault %s",
                 pumpLabels, (const char *)(pump["state"] | ""), pumpFault);
        addSelfTestCheck(checks, "Pump starter", pumpOk, detail, mains ? "fail" : "warn");
    } else {
        addSelfTestCheck(checks, "Pump starter", true,
                         "not fitted in this profile (dryland farming)");
    }

    float currentA = pump["current_a"] | 0.0f;
    int currentWhole = (int)currentA;
    int currentFrac = (int)((currentA - currentWhole) * 100.0f + 0.5f);
    int acsZeroMv = (int)(sensors["acs_zero_mv"] | 1250.0f);
    snprintf(detail, sizeof(detail), "ACS712 reads %d.%02d A · zero %d mV",
             currentWhole, currentFrac, acsZeroMv);
    addSelfTestCheck(checks, "Current sensor", currentA >= 0.0f && currentA <= 30.0f,
                     detail, mains ? "fail" : "warn");

    bool tankFull = sensors["tank_full"] | false;
    snprintf(detail, sizeof(detail), "input %s · %s", tankFull ? "active" : "idle",
             tankFull ? "FULL" : "OK");
    addSelfTestCheck(checks, "Tank float", true, detail);

    bool sumpDry = sensors["sump_dry"] | false;
    snprintf(detail, sizeof(detail), "input %s · %s", sumpDry ? "active" : "idle",
             sumpDry ? "DRY" : "OK");
    addSelfTestCheck(checks, "Sump float", true, detail);

    int passed = 0;
    int warned = 0;
    int failed = 0;
    for (JsonVariant check : checks) {
        const char *state = check["status"] | "";
        if (strcmp(state, "pass") == 0) passed++;
        else if (strcmp(state, "warn") == 0) warned++;
        else failed++;
    }
    int total = passed + warned + failed;

    time_t finishedAt = (time_t)(uint32_t)(status["now"] | 0);
    struct tm tm;
    char finished[32] = "just now";
    if (finishedAt > 0 && localtime_r(&finishedAt, &tm) != nullptr) {
        strftime(finished, sizeof(finished), "%Y-%m-%d %H:%M:%S", &tm);
    }

    const char *result = failed == 0 ? "PASS" : "FAIL";
    char summary[128];
    if (warned) {
        snprintf(summary, sizeof(summary), "%s · %d/%d checks passed, %d warning(s)",
                 result, passed, total, warned);
    } else {
        snprintf(summary, sizeof(summary), "%s · %d/%d checks passed",
                 result, passed, total);
    }
    doc["ok"] = failed == 0;
    doc["result"] = result;
    doc["summary"] = summary;
    doc["passed"] = passed;
    doc["warnings"] = warned;
    doc["failed"] = failed;
    doc["total"] = total;
    doc["finished"] = finished;
    sendJson(doc, 200, response);
    return 0;
}

static int h_config_get(Route &, const struct http_request_ctx *, struct http_response_ctx *response)
{
    JsonDocument d;
    k_mutex_lock(&g_state_lock, K_FOREVER);
    settingsToJson(g_ctrl.config(), d, false);
    k_mutex_unlock(&g_state_lock);
    sendJson(d, 200, response);
    return 0;
}

static int h_config_post(Route &route, const struct http_request_ctx *,
                         struct http_response_ctx *response)
{
    bool reboot = false;
    bool wrtc_changed = false;
    String error;

    // The request document holds the parsed body (~3-4 KiB on the heap for a
    // dashboard-sized config) and the NVS save below builds another one. On
    // the ESP32 the system heap is small, so scope the request document so it
    // is released before settingsSave() and the response are built.
    k_mutex_lock(&g_state_lock, K_FOREVER);
    {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, route.buf, route.len);
        if (err) {
            k_mutex_unlock(&g_state_lock);
            sendError(400, jsonErrText(err), response);
            return 0;
        }

        char old_wifi_ssid[sizeof(g_cfg.wifi_ssid)];
        char old_wifi_pass[sizeof(g_cfg.wifi_pass)];
        char old_hostname[sizeof(g_cfg.hostname)];
        char old_auth_user[sizeof(g_cfg.auth_user)];
        char old_auth_pass[sizeof(g_cfg.auth_pass)];
        bool old_wrtc_enabled = g_cfg.webrtc.enabled;
        char old_wrtc_server[sizeof(g_cfg.webrtc.server)];
        char old_wrtc_stun[sizeof(g_cfg.webrtc.stun)];
        char old_wrtc_turn[sizeof(g_cfg.webrtc.turn)];
        char old_wrtc_turn_user[sizeof(g_cfg.webrtc.turn_user)];
        char old_wrtc_turn_pass[sizeof(g_cfg.webrtc.turn_pass)];
        uint16_t old_wrtc_poll_ms = g_cfg.webrtc.poll_ms;
        copyField(old_wifi_ssid, sizeof(old_wifi_ssid), g_cfg.wifi_ssid);
        copyField(old_wifi_pass, sizeof(old_wifi_pass), g_cfg.wifi_pass);
        copyField(old_hostname, sizeof(old_hostname), g_cfg.hostname);
        copyField(old_auth_user, sizeof(old_auth_user), g_cfg.auth_user);
        copyField(old_auth_pass, sizeof(old_auth_pass), g_cfg.auth_pass);
        copyField(old_wrtc_server, sizeof(old_wrtc_server), g_cfg.webrtc.server);
        copyField(old_wrtc_stun, sizeof(old_wrtc_stun), g_cfg.webrtc.stun);
        copyField(old_wrtc_turn, sizeof(old_wrtc_turn), g_cfg.webrtc.turn);
        copyField(old_wrtc_turn_user, sizeof(old_wrtc_turn_user), g_cfg.webrtc.turn_user);
        copyField(old_wrtc_turn_pass, sizeof(old_wrtc_turn_pass), g_cfg.webrtc.turn_pass);

        if (!settingsFromJson(doc, g_cfg, error)) {
            k_mutex_unlock(&g_state_lock);
            sendError(400, error.c_str(), response);
            return 0;
        }

        reboot = strcmp(old_wifi_ssid, g_cfg.wifi_ssid) != 0 ||
                 strcmp(old_wifi_pass, g_cfg.wifi_pass) != 0 ||
                 strcmp(old_hostname, g_cfg.hostname) != 0 ||
                 strcmp(old_auth_user, g_cfg.auth_user) != 0 ||
                 strcmp(old_auth_pass, g_cfg.auth_pass) != 0;

        // A new control-plane URL or new STUN/TURN overrides must be re-read
        // from the server; otherwise the peer keeps the previous
        // advertisement until the next reboot.
        wrtc_changed = old_wrtc_enabled != g_cfg.webrtc.enabled ||
                 strcmp(old_wrtc_server, g_cfg.webrtc.server) != 0 ||
                 strcmp(old_wrtc_stun, g_cfg.webrtc.stun) != 0 ||
                 strcmp(old_wrtc_turn, g_cfg.webrtc.turn) != 0 ||
                 strcmp(old_wrtc_turn_user, g_cfg.webrtc.turn_user) != 0 ||
                 strcmp(old_wrtc_turn_pass, g_cfg.webrtc.turn_pass) != 0 ||
                 old_wrtc_poll_ms != g_cfg.webrtc.poll_ms;
    } // request document freed here

    bool saved = settingsSave(g_cfg);
    g_ctrl.applyConfig(g_cfg);
    mraf_set_timezone(g_cfg.timezone);
    k_mutex_unlock(&g_state_lock);

    if (wrtc_changed) {
        mraf_webrtc_config_changed();
    }

    // The browser reloads the config after saving, so the reply only carries
    // the save status instead of the whole document again.
    if (!saved) {
        sendError(500, "failed to persist settings", response);
        return 0;
    }
    JsonDocument out;
    out["ok"] = true;
    out["reboot_required"] = reboot;
    sendJson(out, 200, response);
    return 0;
}

static int h_control(Route &route, const struct http_request_ctx *,
                     struct http_response_ctx *response)
{
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, route.buf, route.len);
    if (err) {
        sendError(400, err.c_str(), response);
        return 0;
    }

    mraf_tick_catchup();
    time_t now = mraf_now();
    String error;
    JsonDocument out;
    k_mutex_lock(&g_state_lock, K_FOREVER);
    if (!mraf_apply_control(doc.as<JsonObject>(), now, error)) {
        k_mutex_unlock(&g_state_lock);
        sendError(400, error.c_str(), response);
        return 0;
    }
    g_cfg = g_ctrl.config();
    settingsSave(g_cfg);
    mraf_apply_inputs_locked(mraf_now());
    mraf_build_status(out);
    k_mutex_unlock(&g_state_lock);

    out["ok"] = true;
    sendJson(out, 200, response);
    return 0;
}

static int h_pump_reset(Route &, const struct http_request_ctx *,
                        struct http_response_ctx *response)
{
    JsonDocument out;
    k_mutex_lock(&g_state_lock, K_FOREVER);
    g_ctrl.resetPumpFault(mraf_now());
    mraf_build_status(out);
    k_mutex_unlock(&g_state_lock);
    out["ok"] = true;
    sendJson(out, 200, response);
    return 0;
}

static int h_time(Route &route, const struct http_request_ctx *,
                  struct http_response_ctx *response)
{
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, route.buf, route.len);
    if (err) {
        sendError(400, err.c_str(), response);
        return 0;
    }
    uint64_t epoch = doc["epoch"].as<uint64_t>();
    if (epoch < 1700000000ULL) {
        sendError(400, "epoch must be a Unix timestamp", response);
        return 0;
    }
    mraf_set_now((time_t)epoch);
    mraf_tick_reset();
    if (!mraf_platform_is_simulator()) {
        mraf_platform_rtc_write((time_t)epoch);
    }
    mraf_platform_persistent_time_write((time_t)epoch);

    JsonDocument out;
    k_mutex_lock(&g_state_lock, K_FOREVER);
    mraf_build_status(out);
    k_mutex_unlock(&g_state_lock);
    out["ok"] = true;
    sendJson(out, 200, response);
    return 0;
}

static int h_events(Route &, const struct http_request_ctx *, struct http_response_ctx *response)
{
    JsonDocument d;
    JsonArray ev = d["events"].to<JsonArray>();
    k_mutex_lock(&g_state_lock, K_FOREVER);
    uint8_t count = g_ctrl.eventCount();
    for (uint8_t i = 0; i < count; i++) {
        const EventRecord &e = g_ctrl.event(i);
        JsonObject o = ev.add<JsonObject>();
        o["t"] = (uint32_t)e.t;
        o["time"] = localTimeString(e.t);
        o["src"] = e.src;
        o["msg"] = e.msg;
    }
    k_mutex_unlock(&g_state_lock);
    sendJson(d, 200, response);
    return 0;
}

static int h_scan(Route &, const struct http_request_ctx *, struct http_response_ctx *response)
{
    JsonDocument d;
    mraf_platform_wifi_scan(d);
    sendJson(d, 200, response);
    return 0;
}

static struct k_work_delayable g_reboot_work;

static void rebootWork(struct k_work *work)
{
    (void)work;
    mraf_platform_restart();
}

static int h_reboot(Route &, const struct http_request_ctx *, struct http_response_ctx *response)
{
    JsonDocument d;
    d["ok"] = true;
    d["reboot"] = true;
    sendJson(d, 200, response);
    k_work_reschedule(&g_reboot_work, K_MSEC(600));
    return 0;
}

static void sendEmpty(int status, struct http_response_ctx *response)
{
    response->status = (enum http_status)status;
    response->headers = g_hdr_json;
    response->header_count = 2;
    response->body = (const uint8_t *)"";
    response->body_len = 0;
    response->final_chunk = true;
}

static int h_chat(Route &route, const struct http_request_ctx *,
                  struct http_response_ctx *response)
{
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, route.buf, route.len);
    if (err) {
        sendError(400, err.c_str(), response);
        return 0;
    }
    const char *text = doc["text"] | "";
    if (text[0] == '\0') {
        text = doc["message"] | "";
    }

    MrafIntent intent;
    mraf_intent_parse(text, intent);

    JsonDocument out;
    JsonObject o = out.to<JsonObject>();
    o["input"] = text;
    o["action"] = intent.action;
    o["target"] = intent.target;
    o["relay_id"] = intent.relay_id;
    o["mode"] = intent.mode;
    if (intent.pulse_ms > 0) {
        o["pulse_ms"] = intent.pulse_ms;
    } else {
        o["pulse_ms"] = nullptr;
    }
    if (intent.boost_min > 0) {
        o["boost_min"] = intent.boost_min;
    } else {
        o["boost_min"] = nullptr;
    }
    o["understood"] = intent.understood;
    o["reply"] = intent.reply;

    if (strcmp(intent.action, "control") == 0) {
        mraf_tick_catchup();
        String error;
        k_mutex_lock(&g_state_lock, K_FOREVER);
        bool applied = mraf_intent_apply(intent, mraf_now(), error);
        if (applied) {
            g_cfg = g_ctrl.config();
            settingsSave(g_cfg);
        }
        k_mutex_unlock(&g_state_lock);
        o["applied"] = applied;
        if (!applied) {
            o["error"] = error.c_str();
        }
    } else if (strcmp(intent.action, "status") == 0 ||
               strcmp(intent.action, "data") == 0) {
        k_mutex_lock(&g_state_lock, K_FOREVER);
        JsonDocument st;
        mraf_build_status(st);
        o["status"].set(st.as<JsonObject>());
        k_mutex_unlock(&g_state_lock);
    }
    o["ok"] = true;
    sendJson(out, 200, response);
    return 0;
}

static int h_mcp(Route &route, const struct http_request_ctx *,
                 struct http_response_ctx *response)
{
    JsonDocument req;
    DeserializationError err = deserializeJson(req, route.buf, route.len);
    if (err) {
        JsonDocument e;
        e["jsonrpc"] = "2.0";
        e["id"] = nullptr;
        JsonObject er = e["error"].to<JsonObject>();
        er["code"] = -32700;
        er["message"] = "parse error";
        sendJson(e, 200, response);
        return 0;
    }
    JsonDocument resp;
    if (!mraf_mcp_handle(req, resp)) {
        sendEmpty(202, response);
        return 0;
    }
    sendJson(resp, 200, response);
    return 0;
}

#if defined(CONFIG_MRAF_SIMULATOR)

static bool simParseBody(Route &route, JsonDocument &doc)
{
    if (route.len == 0) {
        return true;
    }
    DeserializationError err = deserializeJson(doc, route.buf, route.len);
    return !err;
}

static void simStatusOk(JsonDocument &out, struct http_response_ctx *response)
{
    mraf_tick_catchup();
    k_mutex_lock(&g_state_lock, K_FOREVER);
    mraf_apply_inputs_locked(mraf_now());
    mraf_build_status(out);
    k_mutex_unlock(&g_state_lock);
    out["ok"] = true;
    sendJson(out, 200, response);
}

static int h_sim_power(Route &route, const struct http_request_ctx *,
                       struct http_response_ctx *response)
{
    JsonDocument in;
    if (!simParseBody(route, in)) {
        sendError(400, "invalid JSON body", response);
        return 0;
    }
    bool present = in["present"] | true;
    mraf_sim_set_power(present);
    JsonDocument out;
    simStatusOk(out, response);
    return 0;
}

static int h_sim_sensors(Route &route, const struct http_request_ctx *,
                         struct http_response_ctx *response)
{
    JsonDocument in;
    if (!simParseBody(route, in)) {
        sendError(400, "invalid JSON body", response);
        return 0;
    }
    bool tank = mraf_sim_tank_full();
    bool sump = mraf_sim_sump_dry();
    if (!in["tank_full"].isNull()) {
        tank = in["tank_full"].as<bool>();
    }
    if (!in["sump_dry"].isNull()) {
        sump = in["sump_dry"].as<bool>();
    }
    mraf_sim_set_sensors(tank, sump);
    JsonDocument out;
    simStatusOk(out, response);
    return 0;
}

static int h_sim_current(Route &route, const struct http_request_ctx *,
                         struct http_response_ctx *response)
{
    JsonDocument in;
    if (!simParseBody(route, in)) {
        sendError(400, "invalid JSON body", response);
        return 0;
    }
    if (in["auto"] | false) {
        mraf_sim_set_current_auto();
    } else if (!in["amps"].isNull()) {
        mraf_sim_set_current(in["amps"].as<float>());
    }
    JsonDocument out;
    simStatusOk(out, response);
    return 0;
}

static int h_sim_speed(Route &route, const struct http_request_ctx *,
                       struct http_response_ctx *response)
{
    JsonDocument in;
    if (!simParseBody(route, in)) {
        sendError(400, "invalid JSON body", response);
        return 0;
    }
    float speed = in["speed"] | 60.0f;
    mraf_set_time_speed((int32_t)(speed * 1000.0f));
    JsonDocument out;
    simStatusOk(out, response);
    return 0;
}

static int h_sim_step(Route &route, const struct http_request_ctx *,
                      struct http_response_ctx *response)
{
    JsonDocument in;
    if (!simParseBody(route, in)) {
        sendError(400, "invalid JSON body", response);
        return 0;
    }
    float seconds = in["seconds"] | 60.0f;
    mraf_advance_time((int64_t)seconds);
    JsonDocument out;
    simStatusOk(out, response);
    return 0;
}

static int h_sim_reset(Route &, const struct http_request_ctx *,
                       struct http_response_ctx *response)
{
    k_mutex_lock(&g_state_lock, K_FOREVER);
    g_ctrl.begin(g_cfg);
    mraf_sim_set_defaults();
    g_ctrl.addEvent("system", "simulator reset", mraf_now());
    k_mutex_unlock(&g_state_lock);
    mraf_tick_reset();
    JsonDocument out;
    simStatusOk(out, response);
    return 0;
}

#endif // CONFIG_MRAF_SIMULATOR

static int h_not_found(Route &, const struct http_request_ctx *, struct http_response_ctx *response)
{
    sendError(404, "not found", response);
    return 0;
}

static void initRoute(RouteId id, RouteFn fn, char *buf, size_t cap, bool no_auth)
{
    Route &r = g_routes[id];
    r.fn = fn;
    r.buf = buf;
    r.cap = cap;
    r.len = 0;
    r.no_auth = no_auth;
    r.overflow = false;
    r.headers_seen = false;
    r.auth_ok = false;
}

static struct http_resource_detail_dynamic d_root = {
    .common = {.bitmask_of_supported_http_methods = BIT(HTTP_GET),
               .type = HTTP_RESOURCE_TYPE_DYNAMIC},
    .cb = route_cb,
    .user_data = &g_routes[R_ROOT],
};
static struct http_resource_detail_dynamic d_health = {
    .common = {.bitmask_of_supported_http_methods = BIT(HTTP_GET),
               .type = HTTP_RESOURCE_TYPE_DYNAMIC},
    .cb = route_cb,
    .user_data = &g_routes[R_HEALTH],
};
static struct http_resource_detail_dynamic d_status = {
    .common = {.bitmask_of_supported_http_methods = BIT(HTTP_GET),
               .type = HTTP_RESOURCE_TYPE_DYNAMIC},
    .cb = route_cb,
    .user_data = &g_routes[R_STATUS],
};
static struct http_resource_detail_dynamic d_selftest = {
    .common = {.bitmask_of_supported_http_methods = BIT(HTTP_POST),
               .type = HTTP_RESOURCE_TYPE_DYNAMIC},
    .cb = route_cb,
    .user_data = &g_routes[R_SELFTEST],
};
static struct http_resource_detail_dynamic d_config = {
    .common = {.bitmask_of_supported_http_methods = BIT(HTTP_GET) | BIT(HTTP_POST),
               .type = HTTP_RESOURCE_TYPE_DYNAMIC},
    .cb = route_cb,
    .user_data = &g_routes[R_CONFIG],
};
static struct http_resource_detail_dynamic d_control = {
    .common = {.bitmask_of_supported_http_methods = BIT(HTTP_POST),
               .type = HTTP_RESOURCE_TYPE_DYNAMIC},
    .cb = route_cb,
    .user_data = &g_routes[R_CONTROL],
};
static struct http_resource_detail_dynamic d_pump_reset = {
    .common = {.bitmask_of_supported_http_methods = BIT(HTTP_POST),
               .type = HTTP_RESOURCE_TYPE_DYNAMIC},
    .cb = route_cb,
    .user_data = &g_routes[R_PUMP_RESET],
};
static struct http_resource_detail_dynamic d_time = {
    .common = {.bitmask_of_supported_http_methods = BIT(HTTP_POST),
               .type = HTTP_RESOURCE_TYPE_DYNAMIC},
    .cb = route_cb,
    .user_data = &g_routes[R_TIME],
};
static struct http_resource_detail_dynamic d_events = {
    .common = {.bitmask_of_supported_http_methods = BIT(HTTP_GET),
               .type = HTTP_RESOURCE_TYPE_DYNAMIC},
    .cb = route_cb,
    .user_data = &g_routes[R_EVENTS],
};
static struct http_resource_detail_dynamic d_scan = {
    .common = {.bitmask_of_supported_http_methods = BIT(HTTP_GET),
               .type = HTTP_RESOURCE_TYPE_DYNAMIC},
    .cb = route_cb,
    .user_data = &g_routes[R_SCAN],
};
static struct http_resource_detail_dynamic d_reboot = {
    .common = {.bitmask_of_supported_http_methods = BIT(HTTP_POST),
               .type = HTTP_RESOURCE_TYPE_DYNAMIC},
    .cb = route_cb,
    .user_data = &g_routes[R_REBOOT],
};
static struct http_resource_detail_dynamic d_wireguard = {
    .common = {.bitmask_of_supported_http_methods = BIT(HTTP_GET) | BIT(HTTP_POST),
               .type = HTTP_RESOURCE_TYPE_DYNAMIC},
    .cb = route_cb,
    .user_data = &g_routes[R_WIREGUARD],
};
static struct http_resource_detail_dynamic d_chat = {
    .common = {.bitmask_of_supported_http_methods = BIT(HTTP_POST),
               .type = HTTP_RESOURCE_TYPE_DYNAMIC},
    .cb = route_cb,
    .user_data = &g_routes[R_CHAT],
};
static struct http_resource_detail_dynamic d_webrtc = {
    .common = {.bitmask_of_supported_http_methods = BIT(HTTP_GET),
               .type = HTTP_RESOURCE_TYPE_DYNAMIC},
    .cb = route_cb,
    .user_data = &g_routes[R_WEBRTC],
};
#if defined(CONFIG_MRAF_MCP)
static struct http_resource_detail_dynamic d_mcp = {
    .common = {.bitmask_of_supported_http_methods = BIT(HTTP_POST),
               .type = HTTP_RESOURCE_TYPE_DYNAMIC},
    .cb = route_cb,
    .user_data = &g_routes[R_MCP],
};
#endif
#if defined(CONFIG_MRAF_SIMULATOR)
static struct http_resource_detail_dynamic d_sim_power = {
    .common = {.bitmask_of_supported_http_methods = BIT(HTTP_POST),
               .type = HTTP_RESOURCE_TYPE_DYNAMIC},
    .cb = route_cb,
    .user_data = &g_routes[R_SIM_POWER],
};
static struct http_resource_detail_dynamic d_sim_sensors = {
    .common = {.bitmask_of_supported_http_methods = BIT(HTTP_POST),
               .type = HTTP_RESOURCE_TYPE_DYNAMIC},
    .cb = route_cb,
    .user_data = &g_routes[R_SIM_SENSORS],
};
static struct http_resource_detail_dynamic d_sim_current = {
    .common = {.bitmask_of_supported_http_methods = BIT(HTTP_POST),
               .type = HTTP_RESOURCE_TYPE_DYNAMIC},
    .cb = route_cb,
    .user_data = &g_routes[R_SIM_CURRENT],
};
static struct http_resource_detail_dynamic d_sim_speed = {
    .common = {.bitmask_of_supported_http_methods = BIT(HTTP_POST),
               .type = HTTP_RESOURCE_TYPE_DYNAMIC},
    .cb = route_cb,
    .user_data = &g_routes[R_SIM_SPEED],
};
static struct http_resource_detail_dynamic d_sim_step = {
    .common = {.bitmask_of_supported_http_methods = BIT(HTTP_POST),
               .type = HTTP_RESOURCE_TYPE_DYNAMIC},
    .cb = route_cb,
    .user_data = &g_routes[R_SIM_STEP],
};
static struct http_resource_detail_dynamic d_sim_reset = {
    .common = {.bitmask_of_supported_http_methods = BIT(HTTP_POST),
               .type = HTTP_RESOURCE_TYPE_DYNAMIC},
    .cb = route_cb,
    .user_data = &g_routes[R_SIM_RESET],
};
#endif

static struct http_resource_detail_dynamic d_not_found = {
    .common = {.bitmask_of_supported_http_methods = BIT(HTTP_GET) | BIT(HTTP_POST),
               .type = HTTP_RESOURCE_TYPE_DYNAMIC},
    .cb = route_cb,
    .user_data = &g_routes[R_NOT_FOUND],
};

HTTP_RESOURCE_DEFINE(res_root, mraf_http, "/", &d_root);
HTTP_RESOURCE_DEFINE(res_health, mraf_http, "/health", &d_health);
HTTP_RESOURCE_DEFINE(res_status, mraf_http, "/api/status", &d_status);
HTTP_RESOURCE_DEFINE(res_selftest, mraf_http, "/api/selftest", &d_selftest);
HTTP_RESOURCE_DEFINE(res_config, mraf_http, "/api/config", &d_config);
HTTP_RESOURCE_DEFINE(res_control, mraf_http, "/api/control", &d_control);
HTTP_RESOURCE_DEFINE(res_pump_reset, mraf_http, "/api/pump/reset", &d_pump_reset);
HTTP_RESOURCE_DEFINE(res_time, mraf_http, "/api/time", &d_time);
HTTP_RESOURCE_DEFINE(res_events, mraf_http, "/api/events", &d_events);
HTTP_RESOURCE_DEFINE(res_scan, mraf_http, "/api/wifi/scan", &d_scan);
HTTP_RESOURCE_DEFINE(res_reboot, mraf_http, "/api/reboot", &d_reboot);
HTTP_RESOURCE_DEFINE(res_wireguard, mraf_http, "/api/wireguard", &d_wireguard);
HTTP_RESOURCE_DEFINE(res_chat, mraf_http, "/api/chat", &d_chat);
HTTP_RESOURCE_DEFINE(res_webrtc, mraf_http, "/api/webrtc", &d_webrtc);
#if defined(CONFIG_MRAF_MCP)
HTTP_RESOURCE_DEFINE(res_mcp, mraf_http, "/mcp", &d_mcp);
#endif
#if defined(CONFIG_MRAF_SIMULATOR)
HTTP_RESOURCE_DEFINE(res_sim_power, mraf_http, "/api/sim/power", &d_sim_power);
HTTP_RESOURCE_DEFINE(res_sim_sensors, mraf_http, "/api/sim/sensors", &d_sim_sensors);
HTTP_RESOURCE_DEFINE(res_sim_current, mraf_http, "/api/sim/current", &d_sim_current);
HTTP_RESOURCE_DEFINE(res_sim_speed, mraf_http, "/api/sim/speed", &d_sim_speed);
HTTP_RESOURCE_DEFINE(res_sim_step, mraf_http, "/api/sim/step", &d_sim_step);
HTTP_RESOURCE_DEFINE(res_sim_reset, mraf_http, "/api/sim/reset", &d_sim_reset);
#endif

HTTP_SERVER_REGISTER_HEADER_CAPTURE(capture_auth, "Authorization");

HTTP_SERVICE_DEFINE(mraf_http, "0.0.0.0", &g_http_port, 4, 4, nullptr,
                    &d_not_found.common, nullptr);

void mraf_api_init(void)
{
#if defined(CONFIG_ARCH_POSIX)
    const char *port_env = getenv("MRAF_HTTP_PORT");
    if (port_env != nullptr) {
        long port = strtol(port_env, nullptr, 10);
        if (port > 0 && port < 65536) {
            g_http_port = (uint16_t)port;
        }
    }
#endif
    initRoute(R_ROOT, h_root, g_buf_small, sizeof(g_buf_small), false);
    initRoute(R_HEALTH, h_health, g_buf_small, sizeof(g_buf_small), true);
    initRoute(R_STATUS, h_status, g_buf_small, sizeof(g_buf_small), false);
    initRoute(R_SELFTEST, h_selftest, g_buf_small, sizeof(g_buf_small), false);
    initRoute(R_CONFIG, h_config_post, g_buf_config, sizeof(g_buf_config), false);
    initRoute(R_CONTROL, h_control, g_buf_control, sizeof(g_buf_control), false);
    initRoute(R_PUMP_RESET, h_pump_reset, g_buf_small, sizeof(g_buf_small), false);
    initRoute(R_TIME, h_time, g_buf_small, sizeof(g_buf_small), false);
    initRoute(R_EVENTS, h_events, g_buf_small, sizeof(g_buf_small), false);
    initRoute(R_SCAN, h_scan, g_buf_small, sizeof(g_buf_small), false);
    initRoute(R_REBOOT, h_reboot, g_buf_small, sizeof(g_buf_small), false);
    initRoute(R_WIREGUARD, h_wireguard_post, g_buf_control, sizeof(g_buf_control), false);
    initRoute(R_CHAT, h_chat, g_buf_control, sizeof(g_buf_control), false);
    initRoute(R_WEBRTC, h_webrtc, g_buf_small, sizeof(g_buf_small), false);
    initRoute(R_MCP, h_mcp, g_buf_config, sizeof(g_buf_config), false);
#if defined(CONFIG_MRAF_SIMULATOR)
    initRoute(R_SIM_POWER, h_sim_power, g_buf_sim, sizeof(g_buf_sim), false);
    initRoute(R_SIM_SENSORS, h_sim_sensors, g_buf_sim, sizeof(g_buf_sim), false);
    initRoute(R_SIM_CURRENT, h_sim_current, g_buf_sim, sizeof(g_buf_sim), false);
    initRoute(R_SIM_SPEED, h_sim_speed, g_buf_sim_speed, sizeof(g_buf_sim_speed), false);
    initRoute(R_SIM_STEP, h_sim_step, g_buf_sim_step, sizeof(g_buf_sim_step), false);
    initRoute(R_SIM_RESET, h_sim_reset, g_buf_small, sizeof(g_buf_small), false);
#endif
    initRoute(R_NOT_FOUND, h_not_found, g_buf_small, sizeof(g_buf_small), false);

    k_work_init_delayable(&g_reboot_work, rebootWork);
}

uint16_t mraf_api_port(void)
{
    return g_http_port;
}

void mraf_api_start(void)
{
    printk("MRAF: HTTP service host=0.0.0.0 port=%d\n", g_http_port);
    int ret = http_server_start();
    if (ret < 0) {
        printk("HTTP server start failed: %d\n", ret);
    } else {
        printk("MRAF: HTTP server listening on port %d\n", g_http_port);
    }
}
