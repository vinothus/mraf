// On-device MCP JSON-RPC endpoint (see include/mcp.h).
#include "mcp.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "intent.h"
#include "mraf_compat.h"
#include "mraf_platform.h"
#include "mraf_state.h"
#include "remote.h"
#include "settings.h"

static void addTool(JsonArray tools, const char *name, const char *desc)
{
    JsonObject t = tools.add<JsonObject>();
    t["name"] = name;
    t["description"] = desc;
    JsonObject schema = t["inputSchema"].to<JsonObject>();
    schema["type"] = "object";
    schema["properties"].to<JsonObject>();
}

static void addParam(JsonArray tools, const char *prop, const char *type,
                     const char *desc, bool required)
{
    JsonObject t = tools[tools.size() - 1];
    JsonObject props = t["inputSchema"]["properties"].to<JsonObject>();
    JsonObject p = props[prop].to<JsonObject>();
    p["type"] = type;
    p["description"] = desc;
    if (required) {
        t["inputSchema"]["required"].to<JsonArray>().add(prop);
    }
}

static void toolsList(JsonDocument &response)
{
    JsonObject result = response["result"].to<JsonObject>();
    JsonArray tools = result["tools"].to<JsonArray>();

    addTool(tools, "get_status", "Read the full controller status document.");
    addTool(tools, "control_relay", "Switch or pulse one relay (1..12).");
    addParam(tools, "relay_id", "integer", "Relay id 1..12", true);
    addParam(tools, "mode", "string", "auto | on | off", true);
    addParam(tools, "pulse_ms", "integer", "Momentary pulse length in ms", false);
    addTool(tools, "control_pump", "Control the pump contactor, optionally boosting.");
    addParam(tools, "mode", "string", "auto | on | off", true);
    addParam(tools, "boost_min", "integer", "Boost duration in minutes", false);
    addTool(tools, "set_all", "Set every output to auto or off.");
    addParam(tools, "mode", "string", "auto | off", true);
    addTool(tools, "apply_config", "Validate and apply a full configuration document.");
    addParam(tools, "config", "object", "ControllerConfig document", true);
    addTool(tools, "wireguard_status", "Show the WireGuard overlay state.");
    addTool(tools, "chat", "Send a natural-language command using the on-device grammar.");
    addParam(tools, "text", "string", "e.g. 'turn on relay 3'", true);
}

static void statusSummary(JsonDocument &out)
{
    time_t now = mraf_now();
    JsonObject o = out.to<JsonObject>();
    o["device"] = g_cfg.device_name;
    o["device_id"] = g_cfg.device_id;
    o["fw"] = FW_VERSION;
    o["now"] = (uint32_t)now;
    o["now_local"] = localTimeString(now);
    o["profile"] = profileName(g_cfg.profile);
    o["power"] = g_ctrl.powerPresent();
    o["battery_v"] = g_ctrl.batteryVoltage();
    o["tank_full"] = g_ctrl.pump().tank_full;
    o["sump_dry"] = g_ctrl.pump().sump_dry;

    JsonObject p = o["pump"].to<JsonObject>();
    p["on"] = g_ctrl.pump().on;
    p["mode"] = modeName(g_ctrl.config().pump.mode);
    p["state"] = g_ctrl.pumpStateName(now);
    p["fault"] = g_ctrl.pump().fault;

    JsonArray relays = o["relays"].to<JsonArray>();
    for (uint8_t i = 0; i < MAX_CHANNELS; i++) {
        JsonObject r = relays.add<JsonObject>();
        r["id"] = i + 1;
        r["name"] = g_ctrl.config().channels[i].name;
        r["on"] = g_ctrl.relay(i).on;
        r["mode"] = modeName(g_ctrl.config().channels[i].mode);
    }
}

static void wireguardStatus(JsonDocument &out)
{
    JsonObject wg = out.to<JsonObject>();
    wg["enabled"] = g_cfg.wireguard.enabled;
    wg["configured"] = g_cfg.wireguard.configured;
    wg["active"] = mraf_platform_wireguard_active();
    wg["interface"] = g_cfg.wireguard.interface;
    wg["address"] = g_cfg.wireguard.address;
    wg["endpoint"] = g_cfg.wireguard.peer_endpoint;
}

static bool callTool(const char *name, JsonObject args, JsonDocument &out, String &error)
{
    time_t now = mraf_now();

    if (strcmp(name, "get_status") == 0) {
        statusSummary(out);
        return true;
    }
    if (strcmp(name, "wireguard_status") == 0) {
        wireguardStatus(out);
        return true;
    }
    if (strcmp(name, "chat") == 0) {
        MrafIntent intent;
        mraf_intent_parse(args["text"] | "", intent);
        JsonObject o = out.to<JsonObject>();
        o["action"] = intent.action;
        o["understood"] = intent.understood;
        o["reply"] = intent.reply;
        if (strcmp(intent.action, "control") == 0) {
            String applyError;
            bool applied = mraf_intent_apply(intent, now, applyError);
            o["applied"] = applied;
            if (!applied) {
                o["error"] = applyError.c_str();
            }
        }
        return true;
    }

    JsonDocument cmdDoc;
    JsonObject cmd = cmdDoc.to<JsonObject>();
    if (strcmp(name, "control_relay") == 0) {
        cmd["target"] = "relay";
        cmd["id"] = args["relay_id"] | 0;
        cmd["mode"] = args["mode"] | "auto";
        if (!args["pulse_ms"].isNull()) {
            cmd["pulse_ms"] = args["pulse_ms"].as<int>();
        }
    } else if (strcmp(name, "control_pump") == 0) {
        cmd["target"] = "pump";
        cmd["mode"] = args["mode"] | "auto";
        if (!args["boost_min"].isNull()) {
            cmd["boost_min"] = args["boost_min"].as<int>();
        }
    } else if (strcmp(name, "set_all") == 0) {
        cmd["target"] = "all";
        cmd["mode"] = args["mode"] | "off";
    } else if (strcmp(name, "apply_config") == 0) {
        if (!args["config"].is<JsonObject>()) {
            error = "config must be an object";
            return false;
        }
        JsonDocument doc;
        doc.set(args["config"]);
        if (!settingsFromJson(doc, g_cfg, error)) {
            return false;
        }
        g_ctrl.applyConfig(g_cfg);
        settingsSave(g_cfg);
        mraf_set_timezone(g_cfg.timezone);
        JsonObject o = out.to<JsonObject>();
        o["applied"] = true;
        return true;
    } else {
        error = "unknown tool";
        return false;
    }

    if (!mraf_apply_control(cmd, now, error)) {
        return false;
    }
    out.to<JsonObject>()["applied"] = true;
    return true;
}

static void toolsCall(JsonDocument &request, JsonDocument &response)
{
    JsonObject params = request["params"].as<JsonObject>();
    const char *name = params["name"] | "";
    JsonDocument emptyArgs;
    JsonObject args = params["arguments"].as<JsonObject>();
    if (!args) {
        args = emptyArgs.to<JsonObject>();
    }

    JsonDocument out;
    String error;
    k_mutex_lock(&g_state_lock, K_FOREVER);
    bool ok = callTool(name, args, out, error);
    k_mutex_unlock(&g_state_lock);

    JsonObject result = response["result"].to<JsonObject>();
    JsonArray content = result["content"].to<JsonArray>();
    JsonObject text = content.add<JsonObject>();
    text["type"] = "text";
    if (ok) {
        static char buf[2048];
        size_t n = serializeJson(out, buf, sizeof(buf));
        text["text"] = n < sizeof(buf) ? buf : "{\"truncated\":true}";
        result["isError"] = false;
    } else {
        text["text"] = error.length() ? error.c_str() : "tool failed";
        result["isError"] = true;
    }
}

bool mraf_mcp_handle(JsonDocument &request, JsonDocument &response)
{
    JsonVariant id = request["id"];
    if (id.isNull()) {
        return false; // notification
    }
    const char *method = request["method"] | "";

    response["jsonrpc"] = "2.0";
    response["id"] = id;

    if (strcmp(method, "initialize") == 0) {
        JsonObject result = response["result"].to<JsonObject>();
        const char *protocol = request["params"]["protocolVersion"] | "2025-06-18";
        result["protocolVersion"] = protocol;
        result["capabilities"].to<JsonObject>()["tools"].to<JsonObject>();
        JsonObject info = result["serverInfo"].to<JsonObject>();
        info["name"] = "mraf-device";
        info["version"] = FW_VERSION;
        result["instructions"] =
            "Control this MRAF farm controller. Use get_status, control_relay, "
            "control_pump, set_all, apply_config, wireguard_status and chat.";
    } else if (strcmp(method, "ping") == 0) {
        response["result"].to<JsonObject>();
    } else if (strcmp(method, "tools/list") == 0) {
        toolsList(response);
    } else if (strcmp(method, "tools/call") == 0) {
        toolsCall(request, response);
    } else {
        JsonObject err = response["error"].to<JsonObject>();
        err["code"] = -32601;
        err["message"] = "method not found";
    }
    return true;
}
