package com.mraf.server.service;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.mraf.server.model.ControllerConfig;
import com.mraf.server.model.RecurringSchedule;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.boot.test.context.SpringBootTest;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

@SpringBootTest(properties = {
        "spring.datasource.url=jdbc:h2:mem:mraf-test;DB_CLOSE_DELAY=-1",
        "spring.jpa.hibernate.ddl-auto=create-drop",
        "mraf.coap.port=0",
        "mraf.token=secret",
        "mraf.server-timezone=Asia/Kolkata"
})
class RemoteStateServiceTest {
    private static final String DEVICE = "ESP32-ABC123";

    @Autowired
    private RemoteStateService service;

    @Autowired
    private ObjectMapper mapper;

    @BeforeEach
    void reset() {
        service.clearAllDevices();
    }

    private JsonNode poll(String json) throws Exception {
        return service.handlePoll(mapper.readTree(json));
    }

    private String register() throws Exception {
        JsonNode res = poll("""
                {"device_id":"%s","device":"Test Farm","fw":"1.1.0","token":"secret",
                 "profile":"agri","register":true,
                 "auth":{"user":"farm","pass":"farm1234"},"poll_interval_sec":60}
                """.formatted(DEVICE));
        assertEquals("register", res.path("request").asText());
        assertEquals("pending", res.path("status").asText());
        assertFalse(res.path("confirmed").asBoolean());
        return res.path("status").asText();
    }

    private String confirmedPoll() {
        return """
                {"device_id":"%s","device":"Test Farm","fw":"1.1.0","token":"secret",
                 "profile":"agri","register":true,
                 "auth":{"user":"farm","pass":"farm1234"},"poll_interval_sec":60}
                """.formatted(DEVICE);
    }

    @Test
    void rejectsWrongToken() {
        assertTrue(service.tokenMatches("secret"));
        assertFalse(service.tokenMatches("wrong"));
    }

    @Test
    void registersAndWaitsForAdminConfirmation() throws Exception {
        assertEquals("pending", register());

        JsonNode overview = service.snapshot();
        assertEquals(1, overview.path("deviceCount").asInt());
        JsonNode device = overview.path("devices").get(0);
        assertEquals(DEVICE, device.path("id").asText());
        assertFalse(device.path("confirmed").asBoolean());
        assertEquals("farm", device.path("remoteUser").asText());
    }

    @Test
    void confirmedDeviceGetsDataRequestAndStoresSnapshot() throws Exception {
        register();
        service.confirmDevice(DEVICE, mapper.createObjectNode());

        JsonNode first = poll(confirmedPoll());
        assertEquals("data", first.path("request").asText());
        assertTrue(first.path("confirmed").asBoolean());
        String id = first.path("id").asText();

        JsonNode second = poll("""
                {"device_id":"%s","token":"secret",
                 "auth":{"user":"farm","pass":"farm1234"},
                 "ack":"%s","result":"data",
                 "data":{"battery_v":13.3,"power":true,"relays":[1,0,0,0,0,0,0,0,0,0,0,0]}}
                """.formatted(DEVICE, id));
        assertEquals("none", second.path("request").asText());
        JsonNode snapshot = service.snapshot(DEVICE);
        assertEquals(13.3, snapshot.path("lastData").path("battery_v").asDouble());
        assertTrue(snapshot.path("online").asBoolean());
        assertEquals("data", snapshot.path("request").path("lastAck").path("result").asText());
    }

    @Test
    void rejectsWrongDeviceCredentials() throws Exception {
        register();
        service.confirmDevice(DEVICE, mapper.createObjectNode());

        JsonNode res = poll("""
                {"device_id":"%s","token":"secret",
                 "auth":{"user":"farm","pass":"wrong"}}
                """.formatted(DEVICE));
        assertEquals("auth", res.path("request").asText());
        assertEquals("denied", res.path("status").asText());
    }

    @Test
    void queuesControlAndAcknowledgesIt() throws Exception {
        register();
        service.confirmDevice(DEVICE, mapper.createObjectNode());

        JsonNode queued = service.queueControl(DEVICE, mapper.readTree("""
                {"target":"relay","id":3,"mode":"on"}
                """));
        String id = queued.path("id").asText();

        JsonNode response = poll(confirmedPoll());
        assertEquals("control", response.path("request").asText());
        assertEquals(id, response.path("id").asText());
        assertEquals("on", response.path("commands").get(0).path("mode").asText());
        assertEquals("on", service.snapshot(DEVICE).path("config")
                .path("channels").get(2).path("mode").asText());

        JsonNode afterAck = poll("""
                {"device_id":"%s","token":"secret",
                 "auth":{"user":"farm","pass":"farm1234"},
                 "ack":"%s","result":"applied"}
                """.formatted(DEVICE, id));
        assertEquals("data", afterAck.path("request").asText());
    }

    @Test
    void queuesConfigWithProfilePulseAndRecurring() throws Exception {
        register();
        service.confirmDevice(DEVICE, mapper.createObjectNode());

        ControllerConfig cfg = ControllerConfig.demoDefaults();
        cfg.profile = "home";
        cfg.channels.get(0).name = "Pump House";
        cfg.channels.get(0).zone = "Utility";
        cfg.channels.get(0).equipment = "motor";
        cfg.channels.get(0).pulseMode = true;
        cfg.channels.get(0).pulseOnMs = 800;
        cfg.channels.get(0).windows.clear();
        cfg.channels.get(0).windows.add(new com.mraf.server.model.ScheduleWindow("05:00", "05:30"));
        RecurringSchedule rs = new RecurringSchedule();
        rs.name = "Weekly soak";
        rs.repeat = "weekly";
        rs.days = 1;
        rs.from = "05:00";
        rs.to = "05:15";
        rs.channelMask = 0b101;
        rs.action = "on";
        cfg.recurring.add(rs);

        String id = service.applyConfig(DEVICE, cfg).path("id").asText();

        JsonNode response = poll(confirmedPoll());
        assertEquals("config", response.path("request").asText());
        assertEquals(id, response.path("id").asText());
        assertEquals("home", response.path("profile").asText());
        assertEquals("Pump House", response.path("channels").get(0).path("name").asText());
        assertEquals("motor", response.path("channels").get(0).path("equipment").asText());
        assertTrue(response.path("channels").get(0).path("pulse_mode").asBoolean());
        assertEquals(12, response.path("channels").size());
        assertEquals("Weekly soak", response.path("recurring").get(0).path("name").asText());
        assertTrue(response.path("pump").isObject());
    }

    @Test
    void rejectsBadSchedule() {
        ControllerConfig cfg = ControllerConfig.demoDefaults();
        cfg.channels.get(0).windows.clear();
        cfg.channels.get(0).windows.add(new com.mraf.server.model.ScheduleWindow("25:00", "06:00"));
        assertThrows(IllegalArgumentException.class, () -> service.applyConfig(DEVICE, cfg));
    }

    @Test
    void adminCanConfirmRejectAndUpdate() throws Exception {
        register();

        JsonNode body = mapper.readTree("{\"profile\":\"industry\",\"poll_interval_sec\":120}");
        JsonNode confirmed = service.confirmDevice(DEVICE, body);
        assertTrue(confirmed.path("confirmed").asBoolean());
        assertEquals("industry", confirmed.path("profile").asText());
        assertEquals(120, confirmed.path("pollIntervalSec").asInt());

        JsonNode rejected = service.rejectDevice(DEVICE);
        assertTrue(rejected.path("rejected").asBoolean());
        assertFalse(rejected.path("confirmed").asBoolean());
    }

    @Test
    void deviceUserLookupFeedsSecurity() throws Exception {
        assertNull(service.findDeviceUser("farm"));
        register();
        service.confirmDevice(DEVICE, mapper.createObjectNode());

        RemoteStateService.DeviceUser user = service.findDeviceUser("farm");
        assertNotNull(user);
        assertEquals(DEVICE, user.deviceId());
        assertEquals(DEVICE, service.findDeviceIdByUser("farm"));
        assertNull(service.findDeviceUser("unknown"));
    }

    @Test
    void persistsDevicesAndQueueAcrossReload() throws Exception {
        register();
        service.confirmDevice(DEVICE, mapper.createObjectNode());
        service.queueControl(DEVICE, mapper.readTree("""
                {"target":"relay","id":2,"mode":"on"}
                """));

        service.reloadFromDatabase();

        JsonNode snapshot = service.snapshot(DEVICE);
        assertTrue(snapshot.path("confirmed").asBoolean());
        assertEquals("farm", snapshot.path("remoteUser").asText());
        assertEquals("on", snapshot.path("config").path("channels").get(1).path("mode").asText());
        assertEquals(1, snapshot.path("request").path("queued").asInt());
    }
}
