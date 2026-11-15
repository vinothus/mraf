package com.mraf.server.wireguard;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.mraf.server.service.RemoteStateService;
import org.springframework.http.HttpStatus;
import org.springframework.http.MediaType;
import org.springframework.security.core.Authentication;
import org.springframework.web.bind.annotation.DeleteMapping;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PathVariable;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RequestParam;
import org.springframework.web.bind.annotation.RestController;
import org.springframework.web.server.ResponseStatusException;

/**
 * REST control plane for the WireGuard overlay. Administrators manage every
 * peer; a confirmed device may fetch its own tunnel configuration.
 */
@RestController
@RequestMapping("/api/wireguard")
public class WireGuardController {
    private final WireGuardService wireguard;
    private final RemoteStateService state;
    private final ObjectMapper mapper;

    public WireGuardController(WireGuardService wireguard, RemoteStateService state,
                               ObjectMapper mapper) {
        this.wireguard = wireguard;
        this.state = state;
        this.mapper = mapper;
    }

    @GetMapping("/status")
    public JsonNode status(Authentication auth) {
        requireAdmin(auth);
        return wireguard.status();
    }

    @GetMapping("/peers")
    public JsonNode peers(Authentication auth) {
        requireAdmin(auth);
        return wireguard.status().path("peers");
    }

    @GetMapping(value = "/server-config", produces = MediaType.TEXT_PLAIN_VALUE)
    public String serverConfig(Authentication auth) {
        requireAdmin(auth);
        return wireguard.renderServerConfig();
    }

    @PostMapping("/apply")
    public JsonNode apply(Authentication auth) {
        requireAdmin(auth);
        try {
            return wireguard.applyKernel();
        } catch (IllegalStateException e) {
            throw new ResponseStatusException(HttpStatus.BAD_REQUEST, e.getMessage(), e);
        }
    }

    @PostMapping("/devices/{id}/peer")
    public JsonNode createPeer(@PathVariable String id,
                               @RequestParam(required = false) String endpoint,
                               Authentication auth) {
        requireAdmin(auth);
        try {
            wireguard.ensurePeer(id);
            ObjectNode config = wireguard.clientConfig(id, endpoint);
            ObjectNode queued = state.queueWireGuard(id, config);
            ObjectNode out = mapper.createObjectNode();
            out.put("ok", true);
            out.set("config", config);
            out.set("request", queued);
            return out;
        } catch (IllegalArgumentException e) {
            throw new ResponseStatusException(HttpStatus.NOT_FOUND, e.getMessage(), e);
        }
    }

    @GetMapping("/devices/{id}/config")
    public JsonNode deviceConfig(@PathVariable String id,
                                 @RequestParam(required = false) String endpoint,
                                 Authentication auth) {
        requireAccess(id, auth);
        if (wireguard.getPeer(id) == null) {
            throw new ResponseStatusException(HttpStatus.NOT_FOUND, "no WireGuard peer for " + id);
        }
        return wireguard.clientConfig(id, endpoint);
    }

    @GetMapping(value = "/devices/{id}/config/wg", produces = MediaType.TEXT_PLAIN_VALUE)
    public String deviceConfigWg(@PathVariable String id,
                                 @RequestParam(required = false) String endpoint,
                                 Authentication auth) {
        requireAccess(id, auth);
        if (wireguard.getPeer(id) == null) {
            throw new ResponseStatusException(HttpStatus.NOT_FOUND, "no WireGuard peer for " + id);
        }
        return wireguard.renderClientConfig(id, endpoint);
    }

    @DeleteMapping("/devices/{id}/peer")
    public JsonNode deletePeer(@PathVariable String id, Authentication auth) {
        requireAdmin(auth);
        wireguard.removePeer(id);
        return mapper.createObjectNode().put("ok", true);
    }

    private void requireAdmin(Authentication auth) {
        if (auth == null || auth.getAuthorities().stream()
                .noneMatch(a -> "ROLE_ADMIN".equals(a.getAuthority()))) {
            throw new ResponseStatusException(HttpStatus.FORBIDDEN, "admin access required");
        }
    }

    private void requireAccess(String id, Authentication auth) {
        if (auth != null && auth.getAuthorities().stream()
                .anyMatch(a -> "ROLE_ADMIN".equals(a.getAuthority()))) {
            return;
        }
        String owned = state.findDeviceIdByUser(auth == null ? "" : auth.getName());
        if (owned == null || !owned.equals(id)) {
            throw new ResponseStatusException(HttpStatus.FORBIDDEN, "device access denied");
        }
    }
}
