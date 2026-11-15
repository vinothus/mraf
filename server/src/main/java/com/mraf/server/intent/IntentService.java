package com.mraf.server.intent;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.mraf.server.service.RemoteStateService;
import org.springframework.stereotype.Service;

/**
 * Applies a parsed {@link IntentParser.Intent} to the controller queue and
 * produces a natural language reply. Shared by the chat REST endpoint and the
 * MCP {@code chat} tool.
 */
@Service
public class IntentService {
    private final RemoteStateService state;
    private final IntentParser parser;
    private final ObjectMapper mapper;

    public IntentService(RemoteStateService state, IntentParser parser, ObjectMapper mapper) {
        this.state = state;
        this.parser = parser;
        this.mapper = mapper;
    }

    public ObjectNode handle(String deviceId, String text) {
        IntentParser.Intent intent = parser.parse(text);
        ObjectNode out = mapper.createObjectNode();
        out.put("deviceId", deviceId);
        out.put("input", text == null ? "" : text);
        out.put("action", intent.action());
        out.put("understood", intent.understood());
        out.put("reply", intent.reply());
        if ("control".equals(intent.action())) {
            ObjectNode command = mapper.createObjectNode();
            command.put("target", intent.target());
            command.put("mode", intent.mode());
            if ("relay".equals(intent.target())) {
                command.put("id", intent.relayId());
            }
            if (intent.pulseMs() != null) {
                command.put("pulse_ms", intent.pulseMs());
            }
            if (intent.boostMin() != null) {
                command.put("boost_min", intent.boostMin());
            }
            out.set("command", command);
            try {
                out.set("queued", state.queueControl(deviceId, command));
            } catch (IllegalArgumentException e) {
                out.put("understood", false);
                out.put("reply", "That command was rejected: " + e.getMessage());
            }
        } else if ("data".equals(intent.action())) {
            out.set("queued", state.queueDataRequest(deviceId));
        } else if ("status".equals(intent.action())) {
            out.set("status", compact(state.snapshot(deviceId)));
        }
        return out;
    }

    private JsonNode compact(JsonNode snapshot) {
        ObjectNode out = mapper.createObjectNode();
        out.put("online", snapshot.path("online").asBoolean());
        out.put("confirmed", snapshot.path("confirmed").asBoolean());
        out.put("profile", snapshot.path("profile").asText(""));
        out.put("queued", snapshot.path("request").path("queued").asInt());
        out.put("inflight", snapshot.path("request").path("inflight").asBoolean());
        JsonNode data = snapshot.path("lastData");
        out.set("lastData", data.isMissingNode() ? mapper.nullNode() : data.deepCopy());
        return out;
    }
}
