package com.mraf.server.mcp;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import org.springframework.boot.autoconfigure.condition.ConditionalOnProperty;
import org.springframework.http.HttpStatus;
import org.springframework.http.MediaType;
import org.springframework.http.ResponseEntity;
import org.springframework.security.core.Authentication;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RestController;

/**
 * Model Context Protocol endpoint (Streamable HTTP, JSON response mode).
 * Implements the JSON-RPC methods an MCP client needs to enumerate and call
 * tools. Authentication reuses the server's HTTP Basic credentials.
 */
@RestController
@ConditionalOnProperty(name = "mraf.mcp.enabled", havingValue = "true", matchIfMissing = true)
public class McpController {
    private static final String DEFAULT_PROTOCOL = "2025-06-18";

    private final McpToolRegistry registry;
    private final ObjectMapper mapper;

    public McpController(McpToolRegistry registry, ObjectMapper mapper) {
        this.registry = registry;
        this.mapper = mapper;
    }

    @PostMapping(value = "/mcp", produces = MediaType.APPLICATION_JSON_VALUE)
    public ResponseEntity<JsonNode> rpc(@RequestBody JsonNode request, Authentication auth) {
        String method = request.path("method").asText("");
        JsonNode id = request.get("id");

        if (id == null || id.isNull()) {
            return ResponseEntity.accepted().build();
        }

        ObjectNode response = mapper.createObjectNode();
        response.put("jsonrpc", "2.0");
        response.set("id", id);

        try {
            switch (method) {
                case "initialize" -> response.set("result", initialize(request.path("params")));
                case "ping" -> response.set("result", mapper.createObjectNode());
                case "tools/list" -> response.set("result", toolsList());
                case "tools/call" -> response.set("result", toolsCall(request.path("params")));
                default -> response.set("error", error(-32601, "method not found: " + method));
            }
        } catch (Exception e) {
            response.set("error", error(-32603, "internal error: " + e.getMessage()));
        }
        return ResponseEntity.ok(response);
    }

    @GetMapping("/mcp")
    public ResponseEntity<String> streamNotSupported() {
        return ResponseEntity.status(HttpStatus.METHOD_NOT_ALLOWED)
                .header("Allow", "POST")
                .body("This MCP server uses JSON response mode; use POST /mcp.");
    }

    private ObjectNode initialize(JsonNode params) {
        ObjectNode result = mapper.createObjectNode();
        String protocol = params.path("protocolVersion").asText("");
        result.put("protocolVersion", protocol.isBlank() ? DEFAULT_PROTOCOL : protocol);
        ObjectNode capabilities = result.putObject("capabilities");
        capabilities.putObject("tools");
        ObjectNode info = result.putObject("serverInfo");
        info.put("name", "mraf-server");
        info.put("version", "1.0.0");
        result.put("instructions", "Control the MRAF ESP32 farm controller. Use list_devices, "
                + "then control_relay / control_pump / chat. WireGuard tools manage the overlay.");
        return result;
    }

    private ObjectNode toolsList() {
        ObjectNode result = mapper.createObjectNode();
        ArrayNode tools = result.putArray("tools");
        registry.listTools().forEach(tools::add);
        return result;
    }

    private ObjectNode toolsCall(JsonNode params) {
        String name = params.path("name").asText("");
        JsonNode args = params.path("arguments");
        ObjectNode result = mapper.createObjectNode();
        ArrayNode content = result.putArray("content");
        try {
            JsonNode output = registry.call(name, args);
            content.addObject().put("type", "text").put("text", pretty(output));
            result.put("isError", false);
        } catch (McpException e) {
            content.addObject().put("type", "text").put("text", e.getMessage());
            result.put("isError", true);
        } catch (Exception e) {
            content.addObject().put("type", "text").put("text", "tool failed: " + e.getMessage());
            result.put("isError", true);
        }
        return result;
    }

    private ObjectNode error(int code, String message) {
        ObjectNode error = mapper.createObjectNode();
        error.put("code", code);
        error.put("message", message);
        return error;
    }

    private String pretty(JsonNode node) {
        try {
            return mapper.writerWithDefaultPrettyPrinter().writeValueAsString(node);
        } catch (Exception e) {
            return node.toString();
        }
    }
}
