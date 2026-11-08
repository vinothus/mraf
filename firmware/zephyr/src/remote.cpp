// Remote CoAP poll thread, ported from the Arduino firmware.
#include "remote.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <zephyr/kernel.h>

#include "coap_client.h"
#include "config.h"
#include "mraf_compat.h"
#include "mraf_platform.h"
#include "mraf_state.h"
#include "settings.h"

static char g_remoteAckId[33];
static char g_remoteAckResult[24];
static bool g_remoteWantData;
static char g_remoteLastId[33];
static char g_remoteLastResult[24];
static int64_t g_remoteLastPollMs;
static time_t g_remoteLastPollAt;
static int g_remoteLastCode;
static bool g_remoteLastOk;
static char g_remoteLastError[64];
static char g_remoteStatus[16] = "off";
static bool g_remoteConfirmed;

static void copyRemoteText(char *dst, size_t n, const char *src)
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

static void setRemoteAck(const char *id, const char *result)
{
    copyRemoteText(g_remoteAckId, sizeof(g_remoteAckId), id);
    copyRemoteText(g_remoteAckResult, sizeof(g_remoteAckResult), result);
}

void mraf_remote_init(void)
{
    g_remoteAckId[0] = '\0';
    g_remoteAckResult[0] = '\0';
    g_remoteLastId[0] = '\0';
    g_remoteLastResult[0] = '\0';
    g_remoteLastError[0] = '\0';
    g_remoteStatus[0] = '\0';
    copyRemoteText(g_remoteStatus, sizeof(g_remoteStatus), "off");
    g_remoteConfirmed = false;
    g_remoteWantData = false;
    g_remoteLastPollMs = 0;
    g_remoteLastPollAt = 0;
    g_remoteLastCode = 0;
    g_remoteLastOk = false;
    if (g_cfg.coap_enabled) {
        copyRemoteText(g_remoteStatus, sizeof(g_remoteStatus), "waiting");
    }
}

bool mraf_remote_confirmed(void)
{
    return g_remoteConfirmed;
}

bool mraf_remote_has_pending(void)
{
    return g_remoteAckId[0] != '\0' || g_remoteWantData;
}

void mraf_remote_build_status(JsonObject rem)
{
    rem["enabled"] = g_cfg.coap_enabled;
    rem["host"] = g_cfg.coap_host;
    rem["port"] = g_cfg.coap_port;
    rem["path"] = g_cfg.coap_path;
    rem["interval_sec"] = g_cfg.coap_interval_sec;
    rem["last_code"] = g_remoteLastCode;
    rem["last_ok"] = g_remoteLastOk;
    rem["last_error"] = g_remoteLastError;
    rem["last_poll"] = (uint32_t)g_remoteLastPollAt;
    rem["pending"] = mraf_remote_has_pending();
    rem["status"] = g_remoteStatus;
    rem["confirmed"] = g_remoteConfirmed;
    rem["device_id"] = g_cfg.device_id;
    rem["user"] = g_cfg.remote_user;
}

static void applyRemoteMeta(JsonObject res, time_t now)
{
    bool changed = false;

    if (!res["confirmed"].isNull()) {
        bool confirmed = res["confirmed"].as<bool>();
        if (confirmed != g_remoteConfirmed) {
            g_remoteConfirmed = confirmed;
            copyRemoteText(g_remoteStatus, sizeof(g_remoteStatus),
                           confirmed ? "confirmed" : "pending");
            g_ctrl.addEvent("remote", confirmed ? "device confirmed" :
                            "waiting for admin", now);
            changed = true;
        }
    }

    const char *profile = res["profile"] | "";
    if (profile[0]) {
        uint8_t p = g_cfg.profile;
        if (strcmp(profile, "agri") == 0) {
            p = PROFILE_AGRI;
        } else if (strcmp(profile, "home") == 0) {
            p = PROFILE_HOME;
        } else if (strcmp(profile, "godown") == 0) {
            p = PROFILE_GODOWN;
        } else if (strcmp(profile, "industry") == 0) {
            p = PROFILE_INDUSTRY;
        }
        if (p != g_cfg.profile) {
            g_cfg.profile = p;
            g_ctrl.applyConfig(g_cfg);
            g_ctrl.addEvent("remote", "profile updated by server", now);
            changed = true;
        }
    }

    int interval = res["poll_interval_sec"] | 0;
    if (interval >= 10 && interval <= 3600 &&
        (uint16_t)interval != g_cfg.coap_interval_sec) {
        g_cfg.coap_interval_sec = (uint16_t)interval;
        g_ctrl.applyConfig(g_cfg);
        g_ctrl.addEvent("remote", "poll interval updated by server", now);
        changed = true;
    }

    if (changed) {
        settingsSave(g_cfg);
    }
}

static void remotePoll(void)
{
    time_t now = mraf_now();
    g_remoteLastPollAt = now;
    if (g_remoteStatus[0] == '\0' || strcmp(g_remoteStatus, "off") == 0) {
        copyRemoteText(g_remoteStatus, sizeof(g_remoteStatus), "waiting");
    }

    static char body[8192];
    static char resp[8192];

    JsonDocument req;
    req["device"] = g_cfg.device_name;
    req["device_id"] = g_cfg.device_id;
    req["fw"] = FW_VERSION;
    req["token"] = g_cfg.coap_token;
    req["time"] = (uint32_t)now;
    req["profile"] = profileName(g_cfg.profile);
    req["confirmed"] = g_remoteConfirmed;
    req["register"] = g_cfg.remote_register;
    req["poll_interval_sec"] = g_cfg.coap_interval_sec;
    JsonObject auth = req["auth"].to<JsonObject>();
    auth["user"] = g_cfg.remote_user;
    auth["pass"] = g_cfg.remote_pass;
    if (g_remoteAckId[0]) {
        req["ack"] = g_remoteAckId;
        req["result"] = g_remoteAckResult;
    }
    if (g_remoteWantData) {
        JsonObject data = req["data"].to<JsonObject>();
        mraf_build_remote_data(data, now);
    }

    size_t blen = serializeJson(req, body, sizeof(body));
    if (blen == 0 || blen >= sizeof(body)) {
        g_remoteLastOk = false;
        copyRemoteText(g_remoteLastError, sizeof(g_remoteLastError),
                       "request too large");
        return;
    }

    int code = coapPost(g_cfg.coap_host, g_cfg.coap_port, g_cfg.coap_path,
                        body, resp, sizeof(resp), 900);
    g_remoteLastCode = code;
    if (code < 64 || code >= 96) {
        g_remoteLastOk = false;
        snprintf(g_remoteLastError, sizeof(g_remoteLastError),
                 "CoAP error %d", code);
        return;
    }

    g_remoteLastOk = true;
    g_remoteLastError[0] = '\0';

    g_remoteAckId[0] = '\0';
    g_remoteAckResult[0] = '\0';
    g_remoteWantData = false;

    if (!resp[0]) {
        return;
    }

    JsonDocument res;
    DeserializationError err = deserializeJson(res, resp);
    if (err) {
        g_remoteLastOk = false;
        copyRemoteText(g_remoteLastError, sizeof(g_remoteLastError),
                       "invalid CoAP JSON");
        return;
    }

    const char *rq = res["request"] | "";
    const char *id = res["id"] | "";

    applyRemoteMeta(res.as<JsonObject>(), now);

    if (strcmp(rq, "register") == 0) {
        const char *status = res["status"] | "pending";
        if (strcmp(status, "rejected") == 0) {
            copyRemoteText(g_remoteStatus, sizeof(g_remoteStatus), "rejected");
            g_remoteConfirmed = false;
            copyRemoteText(g_remoteLastError, sizeof(g_remoteLastError),
                           res["message"] | "registration rejected");
        } else if (strcmp(status, "confirmed") == 0) {
            copyRemoteText(g_remoteStatus, sizeof(g_remoteStatus), "confirmed");
            g_remoteConfirmed = true;
        } else if (strcmp(status, "credentials_required") == 0) {
            copyRemoteText(g_remoteStatus, sizeof(g_remoteStatus), "denied");
            copyRemoteText(g_remoteLastError, sizeof(g_remoteLastError),
                           "server requires device credentials");
        } else {
            copyRemoteText(g_remoteStatus, sizeof(g_remoteStatus), "pending");
            copyRemoteText(g_remoteLastError, sizeof(g_remoteLastError),
                           res["message"] | "waiting for admin confirmation");
        }
        return;
    }
    if (strcmp(rq, "auth") == 0) {
        copyRemoteText(g_remoteStatus, sizeof(g_remoteStatus), "denied");
        g_remoteConfirmed = false;
        copyRemoteText(g_remoteLastError, sizeof(g_remoteLastError),
                       res["message"] | "device credentials rejected");
        return;
    }

    if (strcmp(rq, "control") == 0) {
        if (id[0] && strcmp(id, g_remoteLastId) == 0) {
            setRemoteAck(id, g_remoteLastResult);
            return;
        }

        int applied = 0;
        int failed = 0;
        String error;
        JsonArray cmds = res["commands"].as<JsonArray>();
        for (JsonVariant v : cmds) {
            if (!v.is<JsonObject>()) {
                failed++;
                continue;
            }
            if (mraf_apply_control(v.as<JsonObject>(), now, error)) {
                applied++;
            } else {
                failed++;
            }
        }

        if (applied > 0) {
            g_cfg = g_ctrl.config();
            settingsSave(g_cfg);
        }

        const char *result = (applied > 0 && failed == 0) ? "applied" :
                             (applied > 0 ? "partial" : "rejected");
        if (id[0]) {
            copyRemoteText(g_remoteLastId, sizeof(g_remoteLastId), id);
            copyRemoteText(g_remoteLastResult, sizeof(g_remoteLastResult), result);
        }
        setRemoteAck(id, result);
    } else if (strcmp(rq, "data") == 0) {
        setRemoteAck(id, "data");
        g_remoteWantData = true;
    } else if (strcmp(rq, "config") == 0) {
        if (id[0] && strcmp(id, g_remoteLastId) == 0) {
            setRemoteAck(id, g_remoteLastResult);
            return;
        }

        String error;
        bool ok = settingsFromJson(res, g_cfg, error);
        const char *result = "rejected";
        if (ok) {
            g_ctrl.applyConfig(g_cfg);
            settingsSave(g_cfg);
            mraf_set_timezone(g_cfg.timezone);
            result = "applied";
        } else {
            copyRemoteText(g_remoteLastError, sizeof(g_remoteLastError),
                           error.c_str());
        }
        if (id[0]) {
            copyRemoteText(g_remoteLastId, sizeof(g_remoteLastId), id);
            copyRemoteText(g_remoteLastResult, sizeof(g_remoteLastResult), result);
        }
        setRemoteAck(id, result);
    } else if (strcmp(rq, "none") != 0 && rq[0]) {
        copyRemoteText(g_remoteLastError, sizeof(g_remoteLastError),
                       "unknown remote request");
    }
}

static void remoteThread(void *, void *, void *)
{
    while (1) {
        k_sleep(K_SECONDS(1));
        if (!g_cfg.coap_enabled || !g_cfg.coap_host[0]) {
            continue;
        }
        bool connected = false;
        mraf_platform_net_info(&connected, nullptr, 0, nullptr, 0, nullptr);
        if (!connected || mraf_platform_is_ap_mode()) {
            continue;
        }
        int64_t now_ms = k_uptime_get();
        uint32_t interval_ms = (uint32_t)g_cfg.coap_interval_sec * 1000U;
        if (g_remoteLastPollMs != 0 &&
            now_ms - g_remoteLastPollMs < (int64_t)interval_ms) {
            continue;
        }
        g_remoteLastPollMs = now_ms;
        k_mutex_lock(&g_state_lock, K_FOREVER);
        remotePoll();
        k_mutex_unlock(&g_state_lock);
    }
}

K_THREAD_STACK_DEFINE(g_remote_stack, 6144);
static struct k_thread g_remote_thread;

void mraf_remote_start(void)
{
    k_thread_create(&g_remote_thread, g_remote_stack,
                    K_THREAD_STACK_SIZEOF(g_remote_stack), remoteThread,
                    nullptr, nullptr, nullptr, K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
    k_thread_name_set(&g_remote_thread, "mraf_remote");
}
