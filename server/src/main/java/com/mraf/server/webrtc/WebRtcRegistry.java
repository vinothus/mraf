package com.mraf.server.webrtc;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import org.springframework.stereotype.Service;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Deque;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/**
 * Live registry of WebRTC sessions plus a tiny signalling mailbox.
 *
 * <p>A session is created when a browser or a controller starts signalling for
 * a device id. The two peers exchange SDP and ICE candidates through
 * {@link #post} / {@link #poll}; the actual media/data path is established
 * peer-to-peer and never touches this server. Peers also call {@link #report}
 * so the dashboard can show connection state, RTT and byte counters without the
 * server ever seeing the payload.
 */
@Service
public class WebRtcRegistry {
    public static final String BROWSER = "browser";
    public static final String DEVICE = "device";

    private final ObjectMapper mapper;
    private final WebRtcProperties props;
    private final Map<String, Session> sessions = new ConcurrentHashMap<>();

    public WebRtcRegistry(ObjectMapper mapper, WebRtcProperties props) {
        this.mapper = mapper;
        this.props = props;
    }

    public static String other(String role) {
        return DEVICE.equals(role) ? BROWSER : DEVICE;
    }

    public Session ensure(String deviceId) {
        cleanup();
        return sessions.computeIfAbsent(deviceId, id -> new Session(id));
    }

    public long post(String deviceId, String from, String type, JsonNode body) {
        Session session = ensure(deviceId);
        return session.addSignal(from, type, body);
    }

    public List<JsonNode> poll(String deviceId, String role, long since) {
        Session session = sessions.get(deviceId);
        if (session == null) {
            return List.of();
        }
        session.touch();
        return session.signalsFor(role, since);
    }

    /** Highest sequence number issued for a device (0 if unknown). */
    public long head(String deviceId) {
        Session session = sessions.get(deviceId);
        return session == null ? 0 : session.head();
    }

    public void report(String deviceId, String role, JsonNode body) {
        ensure(deviceId).report(role, body);
    }

    public JsonNode snapshot() {
        cleanup();
        ObjectNode out = mapper.createObjectNode();
        ArrayNode list = out.putArray("sessions");
        for (Session s : sessions.values()) {
            list.add(s.snapshot());
        }
        out.put("count", list.size());
        out.put("stunPort", props.getStun().getPort());
        out.put("ttlSeconds", props.getSignal().getTtlSeconds());
        return out;
    }

    public boolean remove(String deviceId) {
        return sessions.remove(deviceId) != null;
    }

    public int sessionCount() {
        return sessions.size();
    }

    private void cleanup() {
        long ttl = Math.max(30, props.getSignal().getTtlSeconds()) * 1000L;
        long now = System.currentTimeMillis();
        sessions.entrySet().removeIf(e -> now - e.getValue().lastActivity > ttl);
    }

    public final class Session {
        final String deviceId;
        final long createdAt = System.currentTimeMillis();
        volatile long lastActivity = createdAt;
        private long seq = 0;
        private final Deque<ObjectNode> signals = new ArrayDeque<>();
        private final ObjectNode browser = mapper.createObjectNode();
        private final ObjectNode device = mapper.createObjectNode();

        Session(String deviceId) {
            this.deviceId = deviceId;
        }

        void touch() {
            lastActivity = System.currentTimeMillis();
        }

        synchronized long head() {
            return seq;
        }

        synchronized long addSignal(String from, String type, JsonNode body) {
            touch();
            // A fresh browser offer starts a new negotiation: drop any stale
            // offer/answer/candidate traffic so a device that restarts and
            // polls from seq 0 cannot replay an old handshake (which would
            // produce an answer with a mismatched ICE ufrag/pwd).
            if (BROWSER.equals(from) && "offer".equals(type)) {
                signals.clear();
            }
            long id = ++seq;
            ObjectNode node = mapper.createObjectNode();
            node.put("seq", id);
            node.put("from", from);
            node.put("to", other(from));
            node.put("type", type == null ? "" : type);
            node.put("at", System.currentTimeMillis());
            if (body != null && body.isObject()) {
                ObjectNode obj = (ObjectNode) body;
                obj.fields().forEachRemaining(f -> node.set(f.getKey(), f.getValue()));
            }
            signals.addLast(node);
            int max = Math.max(16, props.getSignal().getMaxQueue());
            while (signals.size() > max) {
                signals.removeFirst();
            }
            return id;
        }

        synchronized List<JsonNode> signalsFor(String to, long since) {
            List<JsonNode> out = new ArrayList<>();
            for (ObjectNode s : signals) {
                if (s.path("seq").asLong() > since && to.equals(s.path("to").asText())) {
                    out.add(s.deepCopy());
                }
            }
            return out;
        }

        synchronized void report(String role, JsonNode body) {
            touch();
            ObjectNode target = DEVICE.equals(role) ? device : browser;
            target.removeAll();
            target.put("role", DEVICE.equals(role) ? DEVICE : BROWSER);
            target.put("at", System.currentTimeMillis());
            if (body != null && body.isObject()) {
                body.fields().forEachRemaining(f -> target.set(f.getKey(), f.getValue()));
            }
        }

        synchronized ObjectNode snapshot() {
            ObjectNode node = mapper.createObjectNode();
            node.put("deviceId", deviceId);
            node.put("createdAt", createdAt);
            node.put("lastActivity", lastActivity);
            node.put("seq", seq);
            node.put("signals", signals.size());
            node.set("browser", browser.deepCopy());
            node.set("device", device.deepCopy());
            return node;
        }
    }
}
