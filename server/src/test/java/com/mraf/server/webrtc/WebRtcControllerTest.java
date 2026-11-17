package com.mraf.server.webrtc;

import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.boot.test.autoconfigure.web.servlet.AutoConfigureMockMvc;
import org.springframework.boot.test.context.SpringBootTest;
import org.springframework.http.MediaType;
import org.springframework.security.test.context.support.WithMockUser;
import org.springframework.test.web.servlet.MockMvc;

import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.get;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.post;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.jsonPath;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;

@SpringBootTest(properties = {
        "spring.datasource.url=jdbc:h2:mem:mraf-webrtc;DB_CLOSE_DELAY=-1",
        "spring.jpa.hibernate.ddl-auto=create-drop",
        "mraf.coap.port=0",
        "mraf.token=secret",
        "mraf.webrtc.enabled=true",
        "mraf.webrtc.stun.port=13478",
        "mraf.webrtc.public-host=stun.example.test"
})
@AutoConfigureMockMvc
class WebRtcControllerTest {
    private static final String DEVICE = "SIM-WRTC";

    @Autowired
    private MockMvc mvc;

    @Autowired
    private WebRtcRegistry registry;

    @BeforeEach
    void reset() {
        registry.remove(DEVICE);
    }

    @Test
    void configExposesStunAndSignaling() throws Exception {
        mvc.perform(get("/api/webrtc/config").param("device_id", DEVICE))
                .andExpect(status().isOk())
                .andExpect(jsonPath("$.enabled").value(true))
                .andExpect(jsonPath("$.transport").value("datachannel"))
                .andExpect(jsonPath("$.device_id").value(DEVICE))
                .andExpect(jsonPath("$.iceServers[0].urls").value("stun:stun.example.test:13478"))
                .andExpect(jsonPath("$.iceServers.length()").value(1))
                .andExpect(jsonPath("$.turn").doesNotExist())
                .andExpect(jsonPath("$.signaling.pollMs").isNumber())
                .andExpect(jsonPath("$.maxPayloadBytes").isNumber());
    }

    @Test
    void signalRequiresToken() throws Exception {
        mvc.perform(post("/api/webrtc/signal").contentType(MediaType.APPLICATION_JSON)
                        .content("""
                                {"device_id":"SIM-WRTC","from":"browser","type":"offer","sdp":"x"}
                                """))
                .andExpect(status().isUnauthorized());
    }

    @Test
    void signalRejectsMissingFields() throws Exception {
        mvc.perform(post("/api/webrtc/signal").contentType(MediaType.APPLICATION_JSON)
                        .header("X-Mraf-Token", "secret")
                        .content("""
                                {"device_id":"SIM-WRTC","from":"browser"}
                                """))
                .andExpect(status().isBadRequest());
    }

    @Test
    void signalRelaysOfferToDevice() throws Exception {
        mvc.perform(post("/api/webrtc/signal").contentType(MediaType.APPLICATION_JSON)
                        .header("X-Mraf-Token", "secret")
                        .content("""
                                {"device_id":"SIM-WRTC","from":"browser","type":"offer","sdp":"OFFER"}
                                """))
                .andExpect(status().isOk())
                .andExpect(jsonPath("$.seq").isNumber());

        mvc.perform(get("/api/webrtc/signal")
                        .param("device_id", DEVICE).param("role", "device").param("since", "0")
                        .header("X-Mraf-Token", "secret"))
                .andExpect(status().isOk())
                .andExpect(jsonPath("$.signals[0].type").value("offer"))
                .andExpect(jsonPath("$.signals[0].sdp").value("OFFER"));
    }

    @Test
    @WithMockUser(username = "admin", roles = "ADMIN")
    void reportStoresPeerState() throws Exception {
        mvc.perform(post("/api/webrtc/report").contentType(MediaType.APPLICATION_JSON)
                        .header("X-Mraf-Token", "secret")
                        .content("""
                                {"device_id":"SIM-WRTC","role":"browser","state":"connected","rtt_ms":9}
                                """))
                .andExpect(status().isOk());

        mvc.perform(get("/api/webrtc/sessions"))
                .andExpect(status().isOk())
                .andExpect(jsonPath("$.sessions[0].browser.state").value("connected"));
    }

    @Test
    void sessionsRequireAdmin() throws Exception {
        mvc.perform(get("/api/webrtc/sessions"))
                .andExpect(status().isForbidden());
    }

    @Test
    @WithMockUser(username = "admin", roles = "ADMIN")
    void adminSeesSessionSnapshot() throws Exception {
        registry.post(DEVICE, "browser", "offer", new com.fasterxml.jackson.databind.ObjectMapper()
                .createObjectNode().put("sdp", "x"));
        mvc.perform(get("/api/webrtc/sessions"))
                .andExpect(status().isOk())
                .andExpect(jsonPath("$.count").value(1))
                .andExpect(jsonPath("$.stunPort").isNumber());
    }
}
