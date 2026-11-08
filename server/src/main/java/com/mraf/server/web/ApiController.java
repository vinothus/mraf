package com.mraf.server.web;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.mraf.server.model.ControllerConfig;
import com.mraf.server.service.RemoteStateService;
import org.springframework.http.HttpStatus;
import org.springframework.security.core.Authentication;
import org.springframework.web.bind.annotation.DeleteMapping;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PathVariable;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RestController;
import org.springframework.web.server.ResponseStatusException;

import java.util.LinkedHashMap;
import java.util.Map;

@RestController
@RequestMapping("/api")
public class ApiController {
    private final RemoteStateService state;
    private final ObjectMapper mapper;

    public ApiController(RemoteStateService state, ObjectMapper mapper) {
        this.state = state;
        this.mapper = mapper;
    }

    @GetMapping("/health")
    public Map<String, Object> health() {
        return Map.of(
                "ok", true,
                "service", "mraf-springboot",
                "controllerOnline", state.isOnline(),
                "devices", state.deviceIds().size()
        );
    }

    @GetMapping("/me")
    public Map<String, Object> me(Authentication auth) {
        boolean admin = isAdmin(auth);
        Map<String, Object> out = new LinkedHashMap<>();
        out.put("username", auth.getName());
        out.put("role", admin ? "admin" : "device");
        out.put("deviceId", admin ? null : state.findDeviceIdByUser(auth.getName()));
        return out;
    }

    @GetMapping("/devices")
    public JsonNode devices(Authentication auth) {
        requireAdmin(auth);
        return state.snapshot();
    }

    @GetMapping("/devices/{id}")
    public JsonNode device(@PathVariable String id, Authentication auth) {
        requireAccess(id, auth);
        return state.snapshot(id);
    }

    @PostMapping("/devices/{id}/data-request")
    public JsonNode requestData(@PathVariable String id, Authentication auth) {
        requireAccess(id, auth);
        return state.queueDataRequest(id);
    }

    @PostMapping("/devices/{id}/control")
    public JsonNode control(@PathVariable String id, @RequestBody JsonNode body,
                            Authentication auth) {
        requireAccess(id, auth);
        try {
            return state.queueControl(id, body);
        } catch (IllegalArgumentException e) {
            throw new ResponseStatusException(HttpStatus.BAD_REQUEST, e.getMessage(), e);
        }
    }

    @PostMapping("/devices/{id}/config")
    public JsonNode config(@PathVariable String id, @RequestBody ControllerConfig config,
                           Authentication auth) {
        requireAccess(id, auth);
        try {
            return state.applyConfig(id, config);
        } catch (IllegalArgumentException e) {
            throw new ResponseStatusException(HttpStatus.BAD_REQUEST, e.getMessage(), e);
        }
    }

    @DeleteMapping("/devices/{id}/queue")
    public Map<String, Object> clearQueue(@PathVariable String id, Authentication auth) {
        requireAccess(id, auth);
        state.clearQueue(id);
        return Map.of("ok", true);
    }

    @PostMapping("/devices/{id}/confirm")
    public JsonNode confirm(@PathVariable String id,
                            @RequestBody(required = false) JsonNode body,
                            Authentication auth) {
        requireAdmin(auth);
        try {
            return state.confirmDevice(id, body == null ? mapper.createObjectNode() : body);
        } catch (IllegalArgumentException e) {
            throw new ResponseStatusException(HttpStatus.NOT_FOUND, e.getMessage(), e);
        }
    }

    @PostMapping("/devices/{id}/reject")
    public JsonNode reject(@PathVariable String id, Authentication auth) {
        requireAdmin(auth);
        try {
            return state.rejectDevice(id);
        } catch (IllegalArgumentException e) {
            throw new ResponseStatusException(HttpStatus.NOT_FOUND, e.getMessage(), e);
        }
    }

    @PostMapping("/devices/{id}/update")
    public JsonNode update(@PathVariable String id, @RequestBody JsonNode body,
                           Authentication auth) {
        requireAdmin(auth);
        try {
            return state.updateDevice(id, body);
        } catch (IllegalArgumentException e) {
            throw new ResponseStatusException(HttpStatus.NOT_FOUND, e.getMessage(), e);
        }
    }

    @DeleteMapping("/devices/{id}")
    public Map<String, Object> delete(@PathVariable String id, Authentication auth) {
        requireAdmin(auth);
        state.deleteDevice(id);
        return Map.of("ok", true);
    }

    // Backwards-compatible overview endpoint.
    @GetMapping("/state")
    public JsonNode state(Authentication auth) {
        if (isAdmin(auth)) {
            return state.snapshot();
        }
        String id = state.findDeviceIdByUser(auth.getName());
        if (id == null) {
            throw new ResponseStatusException(HttpStatus.FORBIDDEN, "no device for this user");
        }
        return state.snapshot(id);
    }

    private boolean isAdmin(Authentication auth) {
        return auth != null && auth.getAuthorities().stream()
                .anyMatch(a -> "ROLE_ADMIN".equals(a.getAuthority()));
    }

    private void requireAdmin(Authentication auth) {
        if (!isAdmin(auth)) {
            throw new ResponseStatusException(HttpStatus.FORBIDDEN, "admin access required");
        }
    }

    private void requireAccess(String id, Authentication auth) {
        if (isAdmin(auth)) {
            return;
        }
        String owned = state.findDeviceIdByUser(auth.getName());
        if (owned == null || !owned.equals(id)) {
            throw new ResponseStatusException(HttpStatus.FORBIDDEN, "device access denied");
        }
    }
}
