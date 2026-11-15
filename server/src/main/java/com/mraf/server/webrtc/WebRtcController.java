package com.mraf.server.webrtc;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import jakarta.servlet.http.HttpServletRequest;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.http.HttpStatus;
import org.springframework.security.core.Authentication;
import org.springframework.web.bind.annotation.CrossOrigin;
import org.springframework.web.bind.annotation.DeleteMapping;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PathVariable;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RequestParam;
import org.springframework.web.bind.annotation.RestController;
import org.springframework.web.server.ResponseStatusException;

import java.util.List;

/**
 * WebRTC control plane REST API.
 *
 * <p>Responsibilities:
 * <ul>
 *   <li>{@code GET /config} — hand the browser the STUN URL and signalling
 *       parameters (public, no secrets).</li>
 *   <li>{@code POST /signal} / {@code GET /signal} — relay SDP and ICE between
 *       the browser and the controller. This is the only WebRTC traffic we see;
 *       it carries no dashboard data.</li>
 *   <li>{@code POST /report} — the AJAX call a peer makes to tell us the state
 *       of its peer connection (connected, RTT, bytes). We learn that a session
 *       exists without being able to read it.</li>
 *   <li>{@code GET/DELETE /sessions} — admin view of live sessions.</li>
 * </ul>
 */
@RestController
@RequestMapping("/api/webrtc")
@CrossOrigin(originPatterns = "*")
public class WebRtcController {
    private final WebRtcRegistry registry;
    private final WebRtcProperties props;
    private final StunServer stun;
    private final ObjectMapper mapper;
    private final String token;

    public WebRtcController(WebRtcRegistry registry, WebRtcProperties props, StunServer stun,
                            ObjectMapper mapper, @Value("${mraf.token:demo-token}") String token) {
        this.registry = registry;
        this.props = props;
        this.stun = stun;
        this.mapper = mapper;
        this.token = token;
    }

    @GetMapping("/config")
    public JsonNode config(HttpServletRequest request,
                           @RequestParam(name = "device_id", required = false) String deviceId) {
        String host = props.getPublicHost();
        if (host == null || host.isBlank()) {
            host = request.getServerName();
        }
        int port = props.getStun().getPort();
        String stunUrl = "stun:" + host + ":" + port;

        ObjectNode out = mapper.createObjectNode();
        out.put("enabled", props.isEnabled());
        out.put("transport", "datachannel");
        if (deviceId != null && !deviceId.isBlank()) {
            out.put("device_id", deviceId);
        }
        ArrayNode ice = out.putArray("iceServers");
        ice.addObject().put("urls", stunUrl);
        ObjectNode stunNode = out.putObject("stun");
        stunNode.put("url", stunUrl);
        stunNode.put("host", host);
        stunNode.put("port", port);
        stunNode.put("running", stun.isRunning());
        ObjectNode sig = out.putObject("signaling");
        sig.put("signalUrl", "/api/webrtc/signal");
        sig.put("reportUrl", "/api/webrtc/report");
        sig.put("pollMs", props.getSignal().getPollMs());
        out.put("maxPayloadBytes", props.getSignal().getMaxPayloadBytes());
        return out;
    }

    @PostMapping("/signal")
    public JsonNode signal(@RequestBody JsonNode body, HttpServletRequest request) {
        requireToken(request);
        String deviceId = text(body, "device_id");
        String from = text(body, "from");
        String type = text(body, "type");
        if (deviceId.isEmpty() || type.isEmpty()
                || !(WebRtcRegistry.BROWSER.equals(from) || WebRtcRegistry.DEVICE.equals(from))) {
            throw new ResponseStatusException(HttpStatus.BAD_REQUEST,
                    "device_id, from (browser|device) and type are required");
        }
        if (body.toString().length() > props.getSignal().getMaxPayloadBytes()) {
            throw new ResponseStatusException(HttpStatus.PAYLOAD_TOO_LARGE, "signal too large");
        }
        long seq = registry.post(deviceId, from, type, body);
        ObjectNode out = mapper.createObjectNode();
        out.put("ok", true);
        out.put("seq", seq);
        return out;
    }

    @GetMapping("/signal")
    public JsonNode poll(@RequestParam("device_id") String deviceId,
                         @RequestParam(name = "role", defaultValue = WebRtcRegistry.BROWSER) String role,
                         @RequestParam(name = "since", defaultValue = "0") long since,
                         HttpServletRequest request) {
        requireToken(request);
        List<JsonNode> signals = registry.poll(deviceId, role, since);
        ObjectNode out = mapper.createObjectNode();
        out.put("ok", true);
        out.put("device_id", deviceId);
        out.put("role", role);
        // Current head of the mailbox: a peer that (re)starts should adopt this
        // as its initial cursor so it never replays a stale handshake.
        out.put("seq", registry.head(deviceId));
        ArrayNode arr = out.putArray("signals");
        signals.forEach(arr::add);
        return out;
    }

    @PostMapping("/report")
    public JsonNode report(@RequestBody JsonNode body, HttpServletRequest request) {
        requireToken(request);
        String deviceId = text(body, "device_id");
        String role = text(body, "role");
        if (deviceId.isEmpty()
                || !(WebRtcRegistry.BROWSER.equals(role) || WebRtcRegistry.DEVICE.equals(role))) {
            throw new ResponseStatusException(HttpStatus.BAD_REQUEST,
                    "device_id and role (browser|device) are required");
        }
        registry.report(deviceId, role, body);
        ObjectNode out = mapper.createObjectNode();
        out.put("ok", true);
        return out;
    }

    @GetMapping("/sessions")
    public JsonNode sessions(Authentication auth) {
        requireAdmin(auth);
        return registry.snapshot();
    }

    @DeleteMapping("/sessions/{id}")
    public JsonNode removeSession(@PathVariable String id, Authentication auth) {
        requireAdmin(auth);
        ObjectNode out = mapper.createObjectNode();
        out.put("ok", registry.remove(id));
        return out;
    }

    private void requireToken(HttpServletRequest request) {
        if (!props.isEnabled()) {
            throw new ResponseStatusException(HttpStatus.SERVICE_UNAVAILABLE, "WebRTC disabled");
        }
        String supplied = request.getHeader("X-Mraf-Token");
        if (supplied == null || supplied.isBlank()) {
            supplied = request.getParameter("token");
        }
        if (supplied == null || !supplied.equals(token)) {
            throw new ResponseStatusException(HttpStatus.UNAUTHORIZED, "invalid signalling token");
        }
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

    private static String text(JsonNode body, String field) {
        if (body == null) {
            return "";
        }
        return body.path(field).asText("").trim();
    }
}
