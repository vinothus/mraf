package com.mraf.server.wireguard;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.mraf.server.db.WireGuardPeerEntity;
import com.mraf.server.service.RemoteStateService;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.boot.test.context.SpringBootTest;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

@SpringBootTest(properties = {
        "spring.datasource.url=jdbc:h2:mem:mraf-wg;DB_CLOSE_DELAY=-1",
        "spring.jpa.hibernate.ddl-auto=create-drop",
        "mraf.coap.port=0",
        "mraf.token=secret",
        "mraf.wireguard.config-dir=target/test-wg",
        "mraf.wireguard.manage-kernel=false",
        "mraf.wireguard.endpoint=vpn.example.com:51820"
})
class WireGuardServiceTest {
    @Autowired
    private WireGuardService wireguard;

    @Autowired
    private RemoteStateService state;

    @Autowired
    private WireGuardKeyService keys;

    @Autowired
    private ObjectMapper mapper;

    @BeforeEach
    void reset() {
        for (WireGuardPeerEntity peer : wireguard.listPeers()) {
            wireguard.removePeer(peer.deviceId);
        }
        state.clearAllDevices();
    }

    @Test
    void allocatesSequentialAddresses() {
        assertEquals("10.66.66.2/32", wireguard.ensurePeer("ESP-A").address);
        assertEquals("10.66.66.3/32", wireguard.ensurePeer("ESP-B").address);
        assertEquals("10.66.66.2/32", wireguard.ensurePeer("ESP-A").address);
    }

    @Test
    void clientConfigCarriesKeyMaterial() {
        ObjectNode config = wireguard.clientConfig("ESP-A", null);
        assertEquals("10.66.66.2/32", config.path("address").asText());
        assertTrue(keys.isValidKey(config.path("private_key").asText()));
        assertEquals("vpn.example.com:51820", config.path("peer").path("endpoint").asText());
        assertEquals(wireguard.getServerPublicKey(), config.path("peer").path("public_key").asText());
        assertTrue(keys.isValidKey(config.path("peer").path("preshared_key").asText()));
    }

    @Test
    void rendersWgQuickConfigs() {
        wireguard.ensurePeer("ESP-A");
        String client = wireguard.renderClientConfig("ESP-A", null);
        assertTrue(client.contains("[Interface]"));
        assertTrue(client.contains("Address = 10.66.66.2/32"));
        assertTrue(client.contains("Endpoint = vpn.example.com:51820"));
        assertTrue(client.contains("PersistentKeepalive = 25"));

        String server = wireguard.renderServerConfig();
        assertTrue(server.contains("[Interface]"));
        assertTrue(server.contains("ListenPort = 51820"));
        assertTrue(server.contains("# ESP-A"));
    }

    @Test
    void removingPeerFreesAddress() {
        wireguard.ensurePeer("ESP-A");
        wireguard.removePeer("ESP-A");
        assertNull(wireguard.getPeer("ESP-A"));
        assertEquals("10.66.66.2/32", wireguard.ensurePeer("ESP-C").address);
    }

    @Test
    void statusReportsPeers() {
        wireguard.ensurePeer("ESP-A");
        ObjectNode status = wireguard.status();
        assertEquals(1, status.path("peerCount").asInt());
        assertFalse(status.path("serverPublicKey").asText().isBlank());
        assertEquals("wg0", status.path("interface").asText());
    }

    @Test
    void confirmingDeviceAutoProvisionsPeerAndQueuesConfig() {
        registerAndConfirm("ESP-Z");
        assertNotNull(wireguard.getPeer("ESP-Z"));
        assertEquals(1, state.snapshot("ESP-Z").path("request").path("queued").asInt());
    }

    @Test
    void kernelApplyRendersOnlyWhenDisabled() {
        wireguard.ensurePeer("ESP-A");
        ObjectNode result = wireguard.applyKernel();
        assertFalse(result.path("applied").asBoolean());
        assertTrue(result.path("message").asText().contains("disabled"));
    }

    private void registerAndConfirm(String id) {
        ObjectNode poll = mapper.createObjectNode();
        poll.put("device_id", id);
        poll.put("device", "Test");
        poll.put("fw", "1.1.0");
        poll.put("register", true);
        ObjectNode auth = poll.putObject("auth");
        auth.put("user", "user-" + id);
        auth.put("pass", "pass1234");
        state.handlePoll(poll);
        state.confirmDevice(id, mapper.createObjectNode());
    }
}
