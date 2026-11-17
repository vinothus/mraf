package com.mraf.server.webrtc;

import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.boot.test.autoconfigure.web.servlet.AutoConfigureMockMvc;
import org.springframework.boot.test.context.SpringBootTest;
import org.springframework.test.web.servlet.MockMvc;

import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.get;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.jsonPath;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;

/**
 * TURN advertisement: the device learns the relay URL and credentials from the
 * same config endpoint as the browser, so either peer can allocate a relay.
 */
@SpringBootTest(properties = {
        "spring.datasource.url=jdbc:h2:mem:mraf-turn;DB_CLOSE_DELAY=-1",
        "spring.jpa.hibernate.ddl-auto=create-drop",
        "mraf.coap.port=0",
        "mraf.token=secret",
        "mraf.webrtc.enabled=true",
        "mraf.webrtc.public-host=turn.example.test",
        "mraf.webrtc.stun.port=13478",
        "mraf.webrtc.turn.enabled=true",
        "mraf.webrtc.turn.host=turn.example.test",
        "mraf.webrtc.turn.port=13479",
        "mraf.webrtc.turn.username=mraf",
        "mraf.webrtc.turn.credential=turn-secret"
})
@AutoConfigureMockMvc
class WebRtcTurnConfigTest {
    @Autowired
    private MockMvc mvc;

    @Test
    void configAdvertisesTurnRelayAndCredentials() throws Exception {
        mvc.perform(get("/api/webrtc/config").param("device_id", "SIM-TURN"))
                .andExpect(status().isOk())
                .andExpect(jsonPath("$.iceServers.length()").value(2))
                .andExpect(jsonPath("$.iceServers[0].urls").value("stun:turn.example.test:13478"))
                .andExpect(jsonPath("$.iceServers[1].urls").value("turn:turn.example.test:13479"))
                .andExpect(jsonPath("$.iceServers[1].username").value("mraf"))
                .andExpect(jsonPath("$.iceServers[1].credential").value("turn-secret"))
                .andExpect(jsonPath("$.turn.enabled").value(true))
                .andExpect(jsonPath("$.turn.host").value("turn.example.test"))
                .andExpect(jsonPath("$.turn.port").value(13479))
                .andExpect(jsonPath("$.turn.username").value("mraf"))
                .andExpect(jsonPath("$.turn.credential").value("turn-secret"));
    }
}
