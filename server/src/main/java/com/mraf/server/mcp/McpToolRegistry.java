package com.mraf.server.mcp;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.mraf.server.intent.IntentService;
import com.mraf.server.model.ControllerConfig;
import com.mraf.server.service.RemoteStateService;
import com.mraf.server.wireguard.WireGuardService;
import org.springframework.stereotype.Service;

import java.util.List;

/**
 * Registry of Model Context Protocol tools. Every controller capability is
 * exposed as an MCP tool so external agents (Claude Desktop, IDE assistants,
 * home automation) can drive the farm through a single standard interface.
 */
@Service
public class McpToolRegistry {
    private final RemoteStateService state;
    private final WireGuardService wireguard;
    private final IntentService intents;
    private final ObjectMapper mapper;

    public McpToolRegistry(RemoteStateService state, WireGuardService wireguard,
                           IntentService intents, ObjectMapper mapper) {
        this.state = state;
        this.wireguard = wireguard;
        this.intents = intents;
        this.mapper = mapper;
    }

    private record Param(String name, String type, String description, boolean required) {
        static Param req(String name, String type, String description) {
            return new Param(name, type, description, true);
        }

        static Param opt(String name, String type, String description) {
            return new Param(name, type, description, false);
        }
    }

    public List<ObjectNode> listTools() {
        return List.of(
                tool("list_devices", "List every registered controller with its state.",
                        List.of()),
                tool("get_device", "Get one controller's full snapshot.",
                        List.of(Param.req("device_id", "string", "Controller id, e.g. ESP32-ABC123"))),
                tool("request_data", "Ask a controller to send a fresh status snapshot.",
                        List.of(Param.req("device_id", "string", "Controller id"))),
                tool("control_relay", "Switch or pulse one relay (1..12).",
                        List.of(
                                Param.req("device_id", "string", "Controller id"),
                                Param.req("relay_id", "integer", "Relay id 1..12"),
                                Param.req("mode", "string", "auto | on | off"),
                                Param.opt("pulse_ms", "integer", "Momentary pulse length in ms (20..10000)"))),
                tool("control_pump", "Control the pump contactor, optionally with a boost window.",
                        List.of(
                                Param.req("device_id", "string", "Controller id"),
                                Param.req("mode", "string", "auto | on | off"),
                                Param.opt("boost_min", "integer", "Boost duration in minutes (0..1440)"))),
                tool("set_all", "Set every output to auto or off.",
                        List.of(
                                Param.req("device_id", "string", "Controller id"),
                                Param.req("mode", "string", "auto | off"))),
                tool("apply_config", "Validate and push a full controller configuration.",
                        List.of(
                                Param.req("device_id", "string", "Controller id"),
                                Param.req("config", "object", "ControllerConfig document"))),
                tool("wireguard_status", "Show the WireGuard overlay status and peers.",
                        List.of()),
                tool("wireguard_provision", "Create a WireGuard peer and queue its tunnel config.",
                        List.of(
                                Param.req("device_id", "string", "Controller id"),
                                Param.opt("endpoint", "string", "Public host:port override"))),
                tool("console_command", "Run a command on the controller's on-device Zephyr shell.",
                        List.of(
                                Param.req("device_id", "string", "Controller id"),
                                Param.req("command", "string", "e.g. 'mraf status' or 'kernel version'"))),
                tool("console_output", "Read the buffered output of the controller's console.",
                        List.of(Param.req("device_id", "string", "Controller id"))),
                tool("chat", "Send a natural-language command using the on-device grammar.",
                        List.of(
                                Param.req("device_id", "string", "Controller id"),
                                Param.req("text", "string", "e.g. 'turn on relay 3'"))));
    }

    public JsonNode call(String name, JsonNode args) {
        JsonNode a = args == null || !args.isObject() ? mapper.createObjectNode() : args;
        try {
            return switch (name) {
                case "list_devices" -> state.snapshot();
                case "get_device" -> state.snapshot(requireDevice(a));
                case "request_data" -> state.queueDataRequest(requireDevice(a));
                case "control_relay" -> controlRelay(a);
                case "control_pump" -> controlPump(a);
                case "set_all" -> setAll(a);
                case "apply_config" -> applyConfig(a);
                case "wireguard_status" -> wireguard.status();
                case "wireguard_provision" -> provision(a);
                case "console_command" -> state.queueConsole(requireDevice(a),
                        a.path("command").asText(""));
                case "console_output" -> state.consoleSnapshot(requireDevice(a));
                case "chat" -> intents.handle(requireDevice(a), a.path("text").asText(""));
                default -> throw new McpException(-32602, "unknown tool: " + name);
            };
        } catch (IllegalArgumentException e) {
            throw new McpException(-32602, e.getMessage());
        }
    }

    private JsonNode controlRelay(JsonNode a) {
        ObjectNode cmd = mapper.createObjectNode();
        cmd.put("target", "relay");
        cmd.put("id", a.path("relay_id").asInt());
        cmd.put("mode", a.path("mode").asText("auto"));
        if (a.hasNonNull("pulse_ms")) {
            cmd.put("pulse_ms", a.path("pulse_ms").asInt());
        }
        return state.queueControl(requireDevice(a), cmd);
    }

    private JsonNode controlPump(JsonNode a) {
        ObjectNode cmd = mapper.createObjectNode();
        cmd.put("target", "pump");
        cmd.put("mode", a.path("mode").asText("auto"));
        if (a.hasNonNull("boost_min")) {
            cmd.put("boost_min", a.path("boost_min").asInt());
        }
        return state.queueControl(requireDevice(a), cmd);
    }

    private JsonNode setAll(JsonNode a) {
        ObjectNode cmd = mapper.createObjectNode();
        cmd.put("target", "all");
        cmd.put("mode", a.path("mode").asText("off"));
        return state.queueControl(requireDevice(a), cmd);
    }

    private JsonNode applyConfig(JsonNode a) {
        ControllerConfig config = mapper.convertValue(a.path("config"), ControllerConfig.class);
        return state.applyConfig(requireDevice(a), config);
    }

    private JsonNode provision(JsonNode a) {
        String id = requireDevice(a);
        wireguard.ensurePeer(id);
        ObjectNode config = wireguard.clientConfig(id, a.path("endpoint").asText(null));
        ObjectNode out = mapper.createObjectNode();
        out.set("config", config);
        out.set("request", state.queueWireGuard(id, config));
        return out;
    }

    private String requireDevice(JsonNode a) {
        String id = a.path("device_id").asText("");
        if (id.isBlank()) {
            throw new McpException(-32602, "device_id is required");
        }
        return id;
    }

    private ObjectNode tool(String name, String description, List<Param> params) {
        ObjectNode t = mapper.createObjectNode();
        t.put("name", name);
        t.put("description", description);
        ObjectNode schema = t.putObject("inputSchema");
        schema.put("type", "object");
        ObjectNode propsNode = schema.putObject("properties");
        ArrayNode required = schema.putArray("required");
        for (Param p : params) {
            ObjectNode prop = propsNode.putObject(p.name());
            prop.put("type", p.type());
            prop.put("description", p.description());
            if (p.required()) {
                required.add(p.name());
            }
        }
        return t;
    }
}
