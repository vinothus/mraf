package com.mraf.server.webrtc;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

import java.util.List;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

class WebRtcRegistryTest {
    private final ObjectMapper mapper = new ObjectMapper();
    private WebRtcProperties props;
    private WebRtcRegistry registry;

    @BeforeEach
    void setup() {
        props = new WebRtcProperties();
        registry = new WebRtcRegistry(mapper, props);
    }

    private ObjectNode signal(String type, String sdp) {
        ObjectNode node = mapper.createObjectNode();
        node.put("device_id", "D1");
        node.put("type", type);
        node.put("sdp", sdp);
        return node;
    }

    @Test
    void relaysOfferToDeviceAndAnswerToBrowser() {
        long offerSeq = registry.post("D1", "browser", "offer", signal("offer", "OFFER"));
        assertTrue(offerSeq > 0);

        List<JsonNode> toDevice = registry.poll("D1", "device", 0);
        assertEquals(1, toDevice.size());
        assertEquals("offer", toDevice.get(0).path("type").asText());
        assertEquals("browser", toDevice.get(0).path("from").asText());
        assertEquals("device", toDevice.get(0).path("to").asText());

        // The browser must not be handed back its own offer.
        assertTrue(registry.poll("D1", "browser", 0).isEmpty());

        registry.post("D1", "device", "answer", signal("answer", "ANSWER"));
        List<JsonNode> toBrowser = registry.poll("D1", "browser", offerSeq);
        assertEquals(1, toBrowser.size());
        assertEquals("answer", toBrowser.get(0).path("type").asText());
        assertEquals("device", toBrowser.get(0).path("from").asText());
    }

    @Test
    void sinceCursorSkipsAlreadySeenSignals() {
        long first = registry.post("D1", "browser", "offer", signal("offer", "A"));
        registry.post("D1", "browser", "candidate", signal("candidate", "B"));
        List<JsonNode> fresh = registry.poll("D1", "device", first);
        assertEquals(1, fresh.size());
        assertEquals("candidate", fresh.get(0).path("type").asText());
    }

    @Test
    void reportIsStoredPerRole() {
        ObjectNode report = mapper.createObjectNode();
        report.put("state", "connected");
        report.put("rtt_ms", 12);
        registry.report("D1", "browser", report);

        JsonNode snapshot = registry.snapshot();
        assertEquals(1, snapshot.path("count").asInt());
        JsonNode session = snapshot.path("sessions").get(0);
        assertEquals("D1", session.path("deviceId").asText());
        assertEquals("connected", session.path("browser").path("state").asText());
        assertEquals(12, session.path("browser").path("rtt_ms").asInt());
        assertFalse(session.path("device").has("state"));
    }

    @Test
    void queueIsTrimmedToConfiguredMaximum() {
        props.getSignal().setMaxQueue(16);
        for (int i = 0; i < 40; i++) {
            registry.post("D1", "browser", "candidate", signal("candidate", "c" + i));
        }
        List<JsonNode> queued = registry.poll("D1", "device", 0);
        assertEquals(16, queued.size());
        assertEquals("c24", queued.get(0).path("sdp").asText());
    }

    @Test
    void headTracksLatestSequence() {
        assertEquals(0, registry.head("D1"));
        long a = registry.post("D1", "browser", "offer", signal("offer", "A"));
        assertEquals(a, registry.head("D1"));
        long b = registry.post("D1", "device", "answer", signal("answer", "B"));
        assertEquals(b, registry.head("D1"));
    }

    @Test
    void freshOfferClearsStaleHandshake() {
        registry.post("D1", "browser", "offer", signal("offer", "OLD"));
        registry.post("D1", "device", "answer", signal("answer", "OLD-ANSWER"));
        registry.post("D1", "browser", "candidate", signal("candidate", "OLD-CAND"));

        // A new negotiation must not replay the previous offer/answer, so a
        // device that restarts and polls from seq 0 only sees the new offer.
        long seq = registry.post("D1", "browser", "offer", signal("offer", "NEW"));
        List<JsonNode> toDevice = registry.poll("D1", "device", 0);
        assertEquals(1, toDevice.size());
        assertEquals("NEW", toDevice.get(0).path("sdp").asText());
        assertEquals(seq, toDevice.get(0).path("seq").asLong());
    }

    @Test
    void removeDropsSession() {
        registry.post("D1", "browser", "offer", signal("offer", "X"));
        assertTrue(registry.remove("D1"));
        assertFalse(registry.remove("D1"));
        assertEquals(0, registry.sessionCount());
    }
}
