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

#include "api.h"
#include "config.h"
#include "mraf_compat.h"
#include "mraf_platform.h"
#include "mraf_state.h"
#include "remote.h"
#include "settings.h"
#include "webui.h"

enum RouteId {
    R_ROOT,
    R_HEALTH,
    R_STATUS,
    R_CONFIG,
    R_CONTROL,
    R_PUMP_RESET,
    R_TIME,
    R_EVENTS,
    R_SCAN,
    R_REBOOT,
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
    size_t n = serializeJson(doc, g_resp, sizeof(g_resp));
    response->status = (enum http_status)status;
    response->headers = status == 401 ? g_hdr_401 : g_hdr_json;
    response->header_count = status == 401 ? 3 : 2;
    response->body = (const uint8_t *)g_resp;
    response->body_len = n;
    response->final_chunk = true;
}

static void sendError(int status, const char *msg, struct http_response_ctx *response)
{
    JsonDocument d;
    d["ok"] = false;
    d["error"] = msg ? msg : "error";
    sendJson(d, status, response);
}

static bool evalAuth(const struct http_request_ctx *request, Route &route)
{
    (void)route;
    if (!request || !request->headers) {
        return false;
    }
    const char *auth = nullptr;
    for (size_t i = 0; i < request->header_count; i++) {
        if (request->headers[i].name &&
            strcasecmp(request->headers[i].name, "Authorization") == 0) {
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

static int h_status(Route &, const struct http_request_ctx *, struct http_response_ctx *response)
{
    mraf_tick_catchup();
    JsonDocument d;
    k_mutex_lock(&g_state_lock, K_FOREVER);
    mraf_build_status(d);
    k_mutex_unlock(&g_state_lock);
    sendJson(d, 200, response);
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
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, route.buf, route.len);
    if (err) {
        sendError(400, err.c_str(), response);
        return 0;
    }

    bool reboot = false;
    JsonDocument out;
    k_mutex_lock(&g_state_lock, K_FOREVER);
    Config old = g_cfg;
    String error;
    if (!settingsFromJson(doc, g_cfg, error)) {
        g_cfg = old;
        k_mutex_unlock(&g_state_lock);
        sendError(400, error.c_str(), response);
        return 0;
    }

    reboot = strcmp(old.wifi_ssid, g_cfg.wifi_ssid) != 0 ||
             strcmp(old.wifi_pass, g_cfg.wifi_pass) != 0 ||
             strcmp(old.hostname, g_cfg.hostname) != 0 ||
             strcmp(old.auth_user, g_cfg.auth_user) != 0 ||
             strcmp(old.auth_pass, g_cfg.auth_pass) != 0;

    settingsSave(g_cfg);
    g_ctrl.applyConfig(g_cfg);
    mraf_set_timezone(g_cfg.timezone);
    settingsToJson(g_cfg, out, false);
    k_mutex_unlock(&g_state_lock);

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
HTTP_RESOURCE_DEFINE(res_config, mraf_http, "/api/config", &d_config);
HTTP_RESOURCE_DEFINE(res_control, mraf_http, "/api/control", &d_control);
HTTP_RESOURCE_DEFINE(res_pump_reset, mraf_http, "/api/pump/reset", &d_pump_reset);
HTTP_RESOURCE_DEFINE(res_time, mraf_http, "/api/time", &d_time);
HTTP_RESOURCE_DEFINE(res_events, mraf_http, "/api/events", &d_events);
HTTP_RESOURCE_DEFINE(res_scan, mraf_http, "/api/wifi/scan", &d_scan);
HTTP_RESOURCE_DEFINE(res_reboot, mraf_http, "/api/reboot", &d_reboot);
#if defined(CONFIG_MRAF_SIMULATOR)
HTTP_RESOURCE_DEFINE(res_sim_power, mraf_http, "/api/sim/power", &d_sim_power);
HTTP_RESOURCE_DEFINE(res_sim_sensors, mraf_http, "/api/sim/sensors", &d_sim_sensors);
HTTP_RESOURCE_DEFINE(res_sim_current, mraf_http, "/api/sim/current", &d_sim_current);
HTTP_RESOURCE_DEFINE(res_sim_speed, mraf_http, "/api/sim/speed", &d_sim_speed);
HTTP_RESOURCE_DEFINE(res_sim_step, mraf_http, "/api/sim/step", &d_sim_step);
HTTP_RESOURCE_DEFINE(res_sim_reset, mraf_http, "/api/sim/reset", &d_sim_reset);
#endif

HTTP_SERVER_REGISTER_HEADER_CAPTURE(capture_auth, "Authorization");

HTTP_SERVICE_DEFINE(mraf_http, nullptr, &g_http_port, 4, 4, nullptr,
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
    initRoute(R_CONFIG, h_config_post, g_buf_config, sizeof(g_buf_config), false);
    initRoute(R_CONTROL, h_control, g_buf_control, sizeof(g_buf_control), false);
    initRoute(R_PUMP_RESET, h_pump_reset, g_buf_small, sizeof(g_buf_small), false);
    initRoute(R_TIME, h_time, g_buf_small, sizeof(g_buf_small), false);
    initRoute(R_EVENTS, h_events, g_buf_small, sizeof(g_buf_small), false);
    initRoute(R_SCAN, h_scan, g_buf_small, sizeof(g_buf_small), false);
    initRoute(R_REBOOT, h_reboot, g_buf_small, sizeof(g_buf_small), false);
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
    int ret = http_server_start();
    if (ret < 0) {
        printk("HTTP server start failed: %d\n", ret);
    }
}
