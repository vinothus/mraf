package com.mraf.server.web;

import com.fasterxml.jackson.databind.JsonNode;
import com.mraf.server.intent.IntentService;
import com.mraf.server.service.RemoteStateService;
import org.springframework.http.HttpStatus;
import org.springframework.security.core.Authentication;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RestController;
import org.springframework.web.server.ResponseStatusException;

import java.util.Map;

/**
 * Grammar-based intent chat. The same deterministic parser also backs the MCP
 * {@code chat} tool and the ESP32 on-device chat, so behaviour is identical
 * whether a command arrives by voice, text or an agent.
 */
@RestController
@RequestMapping("/api/chat")
public class ChatController {
    private final IntentService intents;
    private final RemoteStateService state;

    public ChatController(IntentService intents, RemoteStateService state) {
        this.intents = intents;
        this.state = state;
    }

    @PostMapping
    public JsonNode chat(@RequestBody JsonNode body, Authentication auth) {
        String deviceId = body.path("device_id").asText(body.path("deviceId").asText(""));
        String text = body.path("text").asText(body.path("message").asText(""));
        if (deviceId.isBlank()) {
            throw new ResponseStatusException(HttpStatus.BAD_REQUEST, "device_id is required");
        }
        requireAccess(deviceId, auth);
        return intents.handle(deviceId, text);
    }

    @PostMapping("/help")
    public Map<String, Object> help() {
        return Map.of("reply", "Try: 'turn on relay 3', 'pump off', 'all auto', 'status'.");
    }

    private void requireAccess(String id, Authentication auth) {
        boolean admin = auth != null && auth.getAuthorities().stream()
                .anyMatch(a -> "ROLE_ADMIN".equals(a.getAuthority()));
        if (admin) {
            return;
        }
        String owned = state.findDeviceIdByUser(auth == null ? "" : auth.getName());
        if (owned == null || !owned.equals(id)) {
            throw new ResponseStatusException(HttpStatus.FORBIDDEN, "device access denied");
        }
    }
}
