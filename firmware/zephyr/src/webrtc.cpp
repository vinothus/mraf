// WebRTC peer-to-peer link, ported from the simulator's aiortc peer.
//
// The device is the answerer. This file contains everything that is not
// transport specific:
//   * a tiny HTTP/1.1 client for the server's signalling endpoints
//   * the offer -> answer exchange and periodic status reporting
//   * the data-channel command dispatcher (status/config/control/chat/...)
//
// The DTLS/SCTP transport is reached through mraf_platform_webrtc_*; when it
// reports unavailable (native_sim, or an ESP32 build without CONFIG_MRAF_WEBRTC)
// the module keeps the signalling state machine idle and the dashboard falls
// back to plain HTTP.
#include "webrtc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>

#include "config.h"
#include "controller.h"
#include "intent.h"
#include "mraf_compat.h"
#include "mraf_platform.h"
#include "mraf_state.h"
#include "settings.h"

#ifndef CONFIG_MRAF_WEBRTC_SDP_MAX
#define CONFIG_MRAF_WEBRTC_SDP_MAX 4096
#endif
#ifndef CONFIG_MRAF_WEBRTC_MSG_MAX
#define CONFIG_MRAF_WEBRTC_MSG_MAX 2048
#endif

#if defined(CONFIG_MRAF_WEBRTC)
#define WRT_HTTP_BUF 5120
#define WRT_SDP_MAX CONFIG_MRAF_WEBRTC_SDP_MAX
#define WRT_SDP_BUF 2048
#define WRT_MSG_MAX CONFIG_MRAF_WEBRTC_MSG_MAX
#define WRT_REQ_MAX 4096
#define WRT_STACK_SIZE 6144
#else
// The signalling client stays linked but idle when the transport is disabled,
// so its buffers do not need to reserve scarce DRAM on those builds.
#define WRT_HTTP_BUF 64
#define WRT_SDP_MAX 64
#define WRT_SDP_BUF 64
#define WRT_MSG_MAX 64
#define WRT_REQ_MAX 64
#define WRT_STACK_SIZE 1024
#endif

#define WRT_SIGNAL_PATH "/api/webrtc/signal"
#define WRT_CONFIG_PATH "/api/webrtc/config"
#define WRT_REPORT_PATH "/api/webrtc/report"
#define WRT_REPORT_INTERVAL_MS 2000

struct Url {
    char host[64];
    uint16_t port;
    char path[160];
};

// --- link state -------------------------------------------------------------

static char g_status[16] = "off";       // off|waiting|connecting|connected|error
static char g_lastError[64];
static bool g_connected;
static bool g_configFetched;
static long g_since;                    // highest signalling seq seen
static bool g_synced;                   // false until cursor adopted at head
static int64_t g_lastPollMs;
static int64_t g_lastReportMs;
static time_t g_lastPollAt;
static int g_lastCode;
static uint32_t g_txMessages;
static uint32_t g_rxMessages;
static uint32_t g_txBytes;
static uint32_t g_rxBytes;
static uint32_t g_rttMs;

// Single-slot mailbox for data-channel messages. The transport may deliver the
// rx callback from its own thread, so the message is copied here and drained by
// the WebRTC thread under the state lock.
static char g_rxSlot[WRT_REQ_MAX];
static volatile bool g_rxPending;
// Guards the mailbox: the producer waits for the WebRTC thread to take the
// previous message instead of silently overwriting it.
static struct k_sem g_rxFree;

static void copyWrt(char *dst, size_t n, const char *src)
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

static void onWebRtcMessage(const char *message)
{
    if (!message) {
        return;
    }
    if (k_sem_take(&g_rxFree, K_MSEC(2500)) != 0) {
        return;
    }
    copyWrt(g_rxSlot, sizeof(g_rxSlot), message);
    g_rxPending = true;
}

// --- HTTP client ------------------------------------------------------------

static bool parseUrl(const char *base, const char *suffix, Url &out)
{
    const char *p = base;
    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    } else if (strncmp(p, "https://", 8) == 0) {
        return false;   // TLS is not handled by this minimal client
    }
    const char *slash = strchr(p, '/');
    size_t hlen = slash ? (size_t)(slash - p) : strlen(p);
    if (hlen == 0 || hlen >= sizeof(out.host)) {
        return false;
    }
    memcpy(out.host, p, hlen);
    out.host[hlen] = '\0';
    out.port = 80;
    char *colon = strchr(out.host, ':');
    if (colon) {
        *colon = '\0';
        int port = atoi(colon + 1);
        if (port > 0 && port < 65536) {
            out.port = (uint16_t)port;
        }
    }
    copyWrt(out.path, sizeof(out.path), suffix ? suffix : "/");
    return out.host[0] != '\0';
}

// Spring Boot answers with Transfer-Encoding: chunked; the signalling client
// has no chunk decoder of its own, so strip the chunk framing in place.
static void dechunkBody(char *body)
{
    char *in = body;
    char *out = body;
    while (*in) {
        char *end = nullptr;
        unsigned long len = strtoul(in, &end, 16);
        if (end == in || len == 0) {
            break;
        }
        char *nl = strstr(end, "\r\n");
        if (!nl) {
            break;
        }
        in = nl + 2;
        size_t avail = strlen(in);
        size_t take = len < avail ? len : avail;
        memmove(out, in, take);
        out += take;
        in += take;
        if (in[0] == '\r' && in[1] == '\n') {
            in += 2;
        }
    }
    *out = '\0';
}

// Perform one request and return the HTTP status code (or a negative socket
// error). The response body is compacted to the start of the shared buffer and
// returned through out_body (may be empty). The connection is closed by the
// client, so the body is read until EOF. Reusing one buffer for the whole HTTP
// exchange keeps two 5 KiB staging areas out of ESP32 DRAM.
static char g_httpBuf[WRT_HTTP_BUF];

static int httpRequest(const char *method, const Url &url, const char *token,
                       const char *body, char **out_body, int timeout_ms)
{
    char *rx = g_httpBuf;
    if (out_body) {
        *out_body = nullptr;
    }

    struct zsock_addrinfo hints = {};
    struct zsock_addrinfo *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (zsock_getaddrinfo(url.host, nullptr, &hints, &res) != 0 || !res) {
        return -3;
    }
    struct sockaddr_in addr;
    memcpy(&addr, res->ai_addr, sizeof(addr));
    addr.sin_port = htons(url.port);
    zsock_freeaddrinfo(res);

    int fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return -4;
    }
    if (zsock_connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        zsock_close(fd);
        return -4;
    }

    size_t blen = body ? strlen(body) : 0;
    char head[512];
    int hl = snprintf(head, sizeof(head),
                      "%s %s HTTP/1.1\r\n"
                      "Host: %s\r\n"
                      "Connection: close\r\n"
                      "Content-Type: application/json\r\n"
                      "X-Mraf-Token: %s\r\n"
                      "Content-Length: %zu\r\n\r\n",
                      method, url.path, url.host, token ? token : "", blen);
    if (hl <= 0 || hl >= (int)sizeof(head)) {
        zsock_close(fd);
        return -1;
    }

    if (zsock_send(fd, head, (size_t)hl, 0) < 0) {
        zsock_close(fd);
        return -5;
    }
    if (blen && zsock_send(fd, body, blen, 0) < 0) {
        zsock_close(fd);
        return -5;
    }

    size_t total = 0;
    int64_t start = k_uptime_get();
    while (k_uptime_get() - start < (int64_t)timeout_ms && total < sizeof(g_httpBuf) - 1) {
        struct zsock_pollfd pfd = {.fd = fd, .events = ZSOCK_POLLIN};
        int pr = zsock_poll(&pfd, 1, 100);
        if (pr < 0) {
            break;
        }
        if (pr == 0) {
            continue;
        }
        int got = zsock_recv(fd, rx + total, sizeof(g_httpBuf) - 1 - total, 0);
        if (got <= 0) {
            break;
        }
        total += (size_t)got;
    }
    zsock_close(fd);
    rx[total] = '\0';

    int code = 0;
    if (strncmp(rx, "HTTP/", 5) == 0) {
        code = atoi(rx + 9);
    }

    if (out_body) {
        char *sep = strstr(rx, "\r\n\r\n");
        if (sep) {
            bool chunked = strstr(rx, "Transfer-Encoding: chunked") ||
                           strstr(rx, "transfer-encoding: chunked");
            char *bodyp = sep + 4;
            size_t blen = strlen(bodyp);
            memmove(rx, bodyp, blen + 1);
            if (chunked) {
                dechunkBody(rx);
            }
            *out_body = rx;
        }
    }
    return code;
}

static bool httpJson(const char *method, const Url &url, const char *token,
                     const char *body, JsonDocument &out)
{
    char *resp = nullptr;
    int code = httpRequest(method, url, token, body, &resp, 1500);
    g_lastCode = code;
    if (code < 200 || code >= 300 || !resp || !resp[0]) {
        return false;
    }
    DeserializationError err = deserializeJson(out, resp);
    return !err;
}

// --- signalling -------------------------------------------------------------
//
// Network I/O runs *outside* the state lock: only the short mutations of the
// shared link state take it. Holding the lock across a TCP connect would stall
// the HTTP dashboard for as long as the control plane is unreachable.

struct Ctx {
    char server[64];
    char token[48];
    char deviceId[24];
};

static void snapshotCtx(Ctx &ctx)
{
    k_mutex_lock(&g_state_lock, K_FOREVER);
    copyWrt(ctx.server, sizeof(ctx.server), g_cfg.webrtc.server);
    copyWrt(ctx.token, sizeof(ctx.token), g_cfg.coap_token);
    copyWrt(ctx.deviceId, sizeof(ctx.deviceId), g_cfg.device_id);
    k_mutex_unlock(&g_state_lock);
}

static void fetchConfig(const Ctx &ctx)
{
    Url url;
    char suffix[96];
    snprintf(suffix, sizeof(suffix), "%s?device_id=%s", WRT_CONFIG_PATH,
             ctx.deviceId);
    if (!parseUrl(ctx.server, suffix, url)) {
        copyWrt(g_lastError, sizeof(g_lastError), "invalid webrtc server URL");
        return;
    }
    JsonDocument doc;
    if (!httpJson("GET", url, ctx.token, nullptr, doc)) {
        copyWrt(g_lastError, sizeof(g_lastError), "webrtc config fetch failed");
        return;
    }
    if (doc["enabled"].is<bool>() && !doc["enabled"].as<bool>()) {
        copyWrt(g_lastError, sizeof(g_lastError), "webrtc disabled on server");
        return;
    }

    const char *host = "";
    int port = 0;
    JsonObject stun = doc["stun"].as<JsonObject>();
    if (stun) {
        host = stun["host"] | "";
        port = stun["port"] | 0;
    }

    const char *turnHost = "";
    int turnPort = 0;
    const char *turnUser = "";
    const char *turnPass = "";
    JsonObject turn = doc["turn"].as<JsonObject>();
    if (turn) {
        turnHost = turn["host"] | "";
        turnPort = turn["port"] | 0;
        turnUser = turn["username"] | "";
        turnPass = turn["credential"] | "";
    }

    k_mutex_lock(&g_state_lock, K_FOREVER);
    if (host[0] && port > 0 && !g_cfg.webrtc.stun[0]) {
        snprintf(g_cfg.webrtc.stun, sizeof(g_cfg.webrtc.stun),
                 "stun:%s:%d", host, port);
    }
    if (turnHost[0] && turnPort > 0) {
        if (!g_cfg.webrtc.turn[0]) {
            snprintf(g_cfg.webrtc.turn, sizeof(g_cfg.webrtc.turn),
                     "turn:%s:%d", turnHost, turnPort);
        }
        if (!g_cfg.webrtc.turn_user[0] && turnUser[0]) {
            copyWrt(g_cfg.webrtc.turn_user, sizeof(g_cfg.webrtc.turn_user),
                    turnUser);
        }
        if (!g_cfg.webrtc.turn_pass[0] && turnPass[0]) {
            copyWrt(g_cfg.webrtc.turn_pass, sizeof(g_cfg.webrtc.turn_pass),
                    turnPass);
        }
    }
    g_configFetched = true;
    g_lastError[0] = '\0';
    k_mutex_unlock(&g_state_lock);
}

static void postSignal(const Ctx &ctx, const char *type, const char *sdp)
{
    Url url;
    if (!parseUrl(ctx.server, WRT_SIGNAL_PATH, url)) {
        return;
    }
    JsonDocument body;
    body["device_id"] = ctx.deviceId;
    body["from"] = "device";
    body["type"] = type;
    if (sdp && sdp[0]) {
        body["sdp"] = sdp;
    }
    // JSON escaping (quotes, CRLF) can roughly double an SDP body.
    static char buf[WRT_SDP_BUF];
    size_t n = serializeJson(body, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) {
        return;
    }
    JsonDocument out;
    httpJson("POST", url, ctx.token, buf, out);
}

static void handleOffer(const Ctx &ctx, const char *sdp)
{
    if (!sdp || !sdp[0]) {
        return;
    }
    static char answer[WRT_SDP_MAX];
    answer[0] = '\0';
    // A fresh offer supersedes any previous link: tear the old peer down so a
    // browser reconnect starts from a clean DTLS/ICE state.
    mraf_platform_webrtc_stop();
    if (!mraf_platform_webrtc_start(sdp, answer, sizeof(answer))) {
        k_mutex_lock(&g_state_lock, K_FOREVER);
        copyWrt(g_lastError, sizeof(g_lastError), "peer transport rejected offer");
        copyWrt(g_status, sizeof(g_status), "error");
        k_mutex_unlock(&g_state_lock);
        return;
    }
    k_mutex_lock(&g_state_lock, K_FOREVER);
    copyWrt(g_status, sizeof(g_status), "connecting");
    g_ctrl.addEvent("webrtc", "answered browser offer", mraf_now());
    k_mutex_unlock(&g_state_lock);
    postSignal(ctx, "answer", answer);
}

static void pollSignals(const Ctx &ctx)
{
    Url url;
    char suffix[128];
    snprintf(suffix, sizeof(suffix), "%s?device_id=%s&role=device&since=%ld",
             WRT_SIGNAL_PATH, ctx.deviceId, g_since);
    if (!parseUrl(ctx.server, suffix, url)) {
        return;
    }

    JsonDocument doc;
    if (!httpJson("GET", url, ctx.token, nullptr, doc)) {
        return;
    }
    g_lastError[0] = '\0';

    // On (re)start adopt the mailbox head as the cursor so a stale offer from
    // a previous session is never replayed (that would yield an answer with a
    // mismatched ICE ufrag/pwd).
    if (!g_synced) {
        long head = doc["seq"] | 0L;
        if (head > g_since) {
            g_since = head;
        }
        g_synced = true;
        return;
    }

    // The registry is in memory, so a control-plane restart resets the
    // per-device sequence to zero. Without this the device would keep a higher
    // cursor and silently ignore every new offer until it reboots.
    long head = doc["seq"] | 0L;
    if (head < g_since) {
        g_since = head;
    }

    JsonArray signals = doc["signals"].as<JsonArray>();
    for (JsonVariant v : signals) {
        if (!v.is<JsonObject>()) {
            continue;
        }
        JsonObject s = v.as<JsonObject>();
        long seq = s["seq"] | 0L;
        if (seq > g_since) {
            g_since = seq;
        }
        const char *type = s["type"] | "";
        if (strcmp(type, "offer") == 0) {
            handleOffer(ctx, s["sdp"] | "");
        } else if (strcmp(type, "candidate") == 0) {
            const char *cand = s["candidate"] | "";
            if (cand[0]) {
                mraf_platform_webrtc_add_candidate(cand);
            }
        } else if (strcmp(type, "bye") == 0 || strcmp(type, "close") == 0) {
            mraf_platform_webrtc_stop();
            k_mutex_lock(&g_state_lock, K_FOREVER);
            g_connected = false;
            copyWrt(g_status, sizeof(g_status), "waiting");
            g_ctrl.addEvent("webrtc", "browser closed the peer link", mraf_now());
            k_mutex_unlock(&g_state_lock);
        }
    }
}

static void postReport(const Ctx &ctx)
{
    Url url;
    if (!parseUrl(ctx.server, WRT_REPORT_PATH, url)) {
        return;
    }

    bool connected;
    uint32_t rtt, tx, rx, txb, rxb;
    char state[16];
    k_mutex_lock(&g_state_lock, K_FOREVER);
    connected = g_connected;
    rtt = g_rttMs;
    tx = g_txMessages;
    rx = g_rxMessages;
    txb = g_txBytes;
    rxb = g_rxBytes;
    copyWrt(state, sizeof(state), g_status);
    k_mutex_unlock(&g_state_lock);

    JsonDocument body;
    body["device_id"] = ctx.deviceId;
    body["role"] = "device";
    body["connected"] = connected;
    body["transport"] = "datachannel";
    body["state"] = state;
    body["rtt_ms"] = rtt;
    body["tx"] = tx;
    body["rx"] = rx;
    body["bytes"] = txb + rxb;
    static char buf[512];
    size_t n = serializeJson(body, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) {
        return;
    }
    JsonDocument out;
    httpJson("POST", url, ctx.token, buf, out);
}

// --- data-channel dispatch --------------------------------------------------

static void sendReply(JsonDocument &out)
{
#if defined(CONFIG_MRAF_WEBRTC)
    // Serialize straight into the peer's tx buffer: status replies are ~6 KB,
    // and duplicating that buffer here would cost scarce DRAM on the ESP32.
    size_t cap = 0;
    char *dst = mraf_platform_webrtc_tx_begin(&cap);
    if (!dst) {
        return;
    }
    size_t n = serializeJson(out, dst, cap);
    if (n == 0 || n >= cap) {
        (void)mraf_platform_webrtc_tx_end(0);
        return;
    }
    if (mraf_platform_webrtc_tx_end(n) >= 0) {
        g_txMessages++;
        g_txBytes += (uint32_t)n;
    }
#else
    static char buf[WRT_MSG_MAX];
    size_t n = serializeJson(out, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) {
        return;
    }
    if (mraf_platform_webrtc_send(buf) >= 0) {
        g_txMessages++;
        g_txBytes += (uint32_t)n;
    }
#endif
}

static void replyWithStatus(JsonDocument &out)
{
    out["ok"] = true;
    JsonDocument st;
    mraf_build_status(st);
    JsonObject data = out["data"].to<JsonObject>();
    data.set(st.as<JsonObject>());
    data["ok"] = true;
}

static void dispatch(const char *raw)
{
    JsonDocument req;
    if (deserializeJson(req, raw)) {
        return;
    }
    g_rxMessages++;
    g_rxBytes += (uint32_t)strlen(raw);

    const char *cmd = req["cmd"] | "";

    JsonDocument out;
    out["id"] = req["id"];   // echo the caller's id, whatever its type

    if (strcmp(cmd, "ping") == 0) {
        out["ok"] = true;
        JsonObject data = out["data"].to<JsonObject>();
        data["pong"] = req["t"];
        data["transport"] = "webrtc";
    } else if (strcmp(cmd, "status") == 0) {
        replyWithStatus(out);
    } else if (strcmp(cmd, "config") == 0) {
        bool haveBody = !req["body"].isNull();
        JsonDocument bodyDoc;
        if (haveBody) {
            bodyDoc.set(req["body"]);
        }
        bool reboot = false;
        if (haveBody) {
            // ~2.6 KiB; a stack copy here pushes the signalling thread past
            // its budget while the data channel is live.
            static Config old;
            old = g_cfg;
            String error;
            if (!settingsFromJson(bodyDoc, g_cfg, error)) {
                g_cfg = old;
                out["ok"] = false;
                out["error"] = error.c_str();
                sendReply(out);
                return;
            }
            reboot = strcmp(old.wifi_ssid, g_cfg.wifi_ssid) != 0 ||
                     strcmp(old.wifi_pass, g_cfg.wifi_pass) != 0 ||
                     strcmp(old.hostname, g_cfg.hostname) != 0 ||
                     strcmp(old.auth_user, g_cfg.auth_user) != 0 ||
                     strcmp(old.auth_pass, g_cfg.auth_pass) != 0;
            settingsSave(g_cfg);
            g_ctrl.applyConfig(g_cfg);
            mraf_set_timezone(g_cfg.timezone);
        }
        out["ok"] = true;
        JsonDocument cfg;
        settingsToJson(g_cfg, cfg, false);
        out["data"].set(cfg.as<JsonObject>());
        out["reboot_required"] = reboot;
    } else if (strcmp(cmd, "events") == 0) {
        out["ok"] = true;
        JsonObject data = out["data"].to<JsonObject>();
        JsonArray ev = data["events"].to<JsonArray>();
        uint8_t count = g_ctrl.eventCount();
        for (uint8_t i = 0; i < count; i++) {
            const EventRecord &e = g_ctrl.event(i);
            JsonObject o = ev.add<JsonObject>();
            o["t"] = (uint32_t)e.t;
            o["time"] = localTimeString(e.t);
            o["src"] = e.src;
            o["msg"] = e.msg;
        }
    } else if (strcmp(cmd, "wireguard") == 0) {
        out["ok"] = true;
        JsonObject data = out["data"].to<JsonObject>();
        data["enabled"] = g_cfg.wireguard.enabled;
        data["configured"] = g_cfg.wireguard.configured;
        data["active"] = mraf_platform_wireguard_active();
        data["interface"] = g_cfg.wireguard.interface;
        data["address"] = g_cfg.wireguard.address;
        data["endpoint"] = g_cfg.wireguard.peer_endpoint;
    } else if (strcmp(cmd, "control") == 0) {
        mraf_tick_catchup();
        String error;
        JsonObject cmdBody = req["body"].as<JsonObject>();
        bool applied = cmdBody && mraf_apply_control(cmdBody, mraf_now(), error);
        if (applied) {
            g_cfg = g_ctrl.config();
            settingsSave(g_cfg);
        }
        out["ok"] = true;
        JsonDocument st;
        mraf_build_status(st);
        JsonObject data = out["data"].to<JsonObject>();
        data.set(st.as<JsonObject>());
        data["ok"] = applied;
        if (!applied) {
            data["error"] = error.c_str();
        }
    } else if (strcmp(cmd, "chat") == 0) {
        const char *text = req["text"] | "";
        MrafIntent intent;
        mraf_intent_parse(text, intent);
        out["ok"] = true;
        JsonObject data = out["data"].to<JsonObject>();
        data["input"] = text;
        data["action"] = intent.action;
        data["understood"] = intent.understood;
        data["reply"] = intent.reply;
        if (strcmp(intent.action, "control") == 0) {
            mraf_tick_catchup();
            String error;
            bool applied = mraf_intent_apply(intent, mraf_now(), error);
            if (applied) {
                g_cfg = g_ctrl.config();
                settingsSave(g_cfg);
            }
            data["applied"] = applied;
            if (!applied) {
                data["error"] = error.c_str();
            }
        } else if (strcmp(intent.action, "status") == 0 ||
                   strcmp(intent.action, "data") == 0) {
            JsonDocument st;
            mraf_build_status(st);
            data["status"].set(st.as<JsonObject>());
        }
    } else if (strcmp(cmd, "time") == 0) {
        uint64_t epoch = req["epoch"].as<uint64_t>();
        if (epoch >= 1700000000ULL) {
            mraf_set_now((time_t)epoch);
            mraf_tick_reset();
            if (!mraf_platform_is_simulator()) {
                mraf_platform_rtc_write((time_t)epoch);
            }
        }
        replyWithStatus(out);
    } else if (strcmp(cmd, "pump-reset") == 0) {
        g_ctrl.resetPumpFault(mraf_now());
        replyWithStatus(out);
    } else if (strcmp(cmd, "bye") == 0) {
        out["ok"] = true;
        JsonObject data = out["data"].to<JsonObject>();
        data["bye"] = true;
        mraf_platform_webrtc_stop();
        g_connected = false;
        copyWrt(g_status, sizeof(g_status), "waiting");
    } else {
        out["ok"] = false;
        out["error"] = "unknown command";
    }

    sendReply(out);
}

// --- thread -----------------------------------------------------------------

static void webrtcThread(void *, void *, void *)
{
    while (1) {
        k_sleep(K_MSEC(50));

        bool enabled;
        uint32_t poll_ms;
        char server[64];
        k_mutex_lock(&g_state_lock, K_FOREVER);
        enabled = g_cfg.webrtc.enabled;
        poll_ms = g_cfg.webrtc.poll_ms ? g_cfg.webrtc.poll_ms :
                  DEFAULT_WEBRTC_POLL_MS;
        copyWrt(server, sizeof(server), g_cfg.webrtc.server);
        k_mutex_unlock(&g_state_lock);

        if (!enabled || !server[0]) {
            if (strcmp(g_status, "off") != 0) {
                copyWrt(g_status, sizeof(g_status), "off");
            }
            continue;
        }

        bool net_up = false;
        mraf_platform_net_info(&net_up, nullptr, 0, nullptr, 0, nullptr);
        if (!net_up || mraf_platform_is_ap_mode()) {
            continue;
        }

        int64_t now_ms = k_uptime_get();
        if (g_lastPollMs != 0 && now_ms - g_lastPollMs < (int64_t)poll_ms) {
            continue;
        }
        g_lastPollMs = now_ms;

        if (!mraf_platform_webrtc_available()) {
            copyWrt(g_status, sizeof(g_status), "unavailable");
            continue;
        }

        // Drain one inbound data-channel message (dispatch mutates state).
        bool have_rx = false;
        // Static: this is the only writer, and a 4 KiB stack buffer would crowd
        // the signalling thread next to the HTTP/JSON and DTLS setup frames.
        static char rx[WRT_REQ_MAX];
        k_mutex_lock(&g_state_lock, K_FOREVER);
        if (g_rxPending) {
            g_rxPending = false;
            copyWrt(rx, sizeof(rx), g_rxSlot);
            have_rx = true;
            k_sem_give(&g_rxFree);
        }
        k_mutex_unlock(&g_state_lock);
        if (have_rx) {
            k_mutex_lock(&g_state_lock, K_FOREVER);
            dispatch(rx);
            k_mutex_unlock(&g_state_lock);
        }

        Ctx ctx;
        snapshotCtx(ctx);
        if (!g_configFetched) {
            fetchConfig(ctx);
        }
        pollSignals(ctx);

        bool open = mraf_platform_webrtc_open();
        k_mutex_lock(&g_state_lock, K_FOREVER);
        if (open != g_connected) {
            g_connected = open;
            copyWrt(g_status, sizeof(g_status), open ? "connected" : "waiting");
            g_lastReportMs = 0;
            g_ctrl.addEvent("webrtc", open ? "peer link connected" :
                            "peer link closed", mraf_now());
        }
        g_lastPollAt = mraf_now();
        k_mutex_unlock(&g_state_lock);

        if (open && (g_lastReportMs == 0 ||
                     now_ms - g_lastReportMs >= WRT_REPORT_INTERVAL_MS)) {
            g_lastReportMs = now_ms;
            postReport(ctx);
        }
    }
}

K_THREAD_STACK_DEFINE(g_webrtc_stack, WRT_STACK_SIZE);
static struct k_thread g_webrtc_thread;

// --- public API -------------------------------------------------------------

void mraf_webrtc_init(void)
{
    g_status[0] = '\0';
    copyWrt(g_status, sizeof(g_status), "off");
    g_lastError[0] = '\0';
    g_connected = false;
    g_configFetched = false;
    g_since = 0;
    g_synced = false;
    g_lastPollMs = 0;
    g_lastReportMs = 0;
    g_lastPollAt = 0;
    g_lastCode = 0;
    g_txMessages = 0;
    g_rxMessages = 0;
    g_txBytes = 0;
    g_rxBytes = 0;
    g_rttMs = 0;
    g_rxPending = false;
    k_sem_init(&g_rxFree, 1, 1);
    mraf_platform_webrtc_set_rx(onWebRtcMessage);
}

void mraf_webrtc_build_status(JsonObject out)
{
    out["enabled"] = g_cfg.webrtc.enabled;
    out["server"] = g_cfg.webrtc.server;
    out["stun"] = g_cfg.webrtc.stun;
    out["turn"] = g_cfg.webrtc.turn;
    out["poll_ms"] = g_cfg.webrtc.poll_ms;
    out["transport"] = "datachannel";
    out["available"] = mraf_platform_webrtc_available();
    out["connected"] = g_connected;
    out["status"] = g_status;
    out["state"] = g_status;
    out["rtt_ms"] = g_rttMs;
    out["tx"] = g_txMessages;
    out["rx"] = g_rxMessages;
    out["bytes"] = g_txBytes + g_rxBytes;
    out["since"] = (uint32_t)g_since;
    out["last_code"] = g_lastCode;
    out["last_error"] = g_lastError;
    out["last_poll"] = (uint32_t)g_lastPollAt;
    out["device_id"] = g_cfg.device_id;

    struct mraf_ice_info ice;
    mraf_platform_webrtc_ice_info(&ice);
    out["stun_ok"] = ice.stun_ok;
    out["srflx"] = ice.srflx_addr;
    out["relay"] = ice.relay_addr;
    out["relay_allocated"] = ice.turn_allocated;
    out["path"] = ice.relay_path ? "relay" : (g_connected ? "direct" : "");
    if (ice.turn_error[0]) {
        out["turn_error"] = ice.turn_error;
    }
}

void mraf_webrtc_start(void)
{
    k_thread_create(&g_webrtc_thread, g_webrtc_stack,
                    K_THREAD_STACK_SIZEOF(g_webrtc_stack), webrtcThread,
                    nullptr, nullptr, nullptr, K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
    k_thread_name_set(&g_webrtc_thread, "mraf_webrtc");
}

void mraf_webrtc_config_changed(void)
{
    g_configFetched = false;
}
