package com.mraf.server.web;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.mraf.server.service.RemoteStateService;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.boot.test.autoconfigure.web.servlet.AutoConfigureMockMvc;
import org.springframework.boot.test.context.SpringBootTest;
import org.springframework.http.MediaType;
import org.springframework.security.test.context.support.WithMockUser;
import org.springframework.test.web.servlet.MockMvc;

import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.post;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.jsonPath;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;

@SpringBootTest(properties = {
        "spring.datasource.url=jdbc:h2:mem:mraf-chat;DB_CLOSE_DELAY=-1",
        "spring.jpa.hibernate.ddl-auto=create-drop",
        "mraf.coap.port=0",
        "mraf.token=secret",
        "mraf.wireguard.config-dir=target/test-chat-wg"
})
@AutoConfigureMockMvc
class ChatControllerTest {
    private static final String DEVICE = "ESP32-CHAT";

    @Autowired
    private MockMvc mvc;

    @Autowired
    private RemoteStateService state;

    @Autowired
    private ObjectMapper mapper;

    @BeforeEach
    void setup() {
        state.clearAllDevices();
        ObjectNode poll = mapper.createObjectNode();
        poll.put("device_id", DEVICE);
        poll.put("register", true);
        ObjectNode auth = poll.putObject("auth");
        auth.put("user", "chatuser");
        auth.put("pass", "pass1234");
        state.handlePoll(poll);
        state.confirmDevice(DEVICE, mapper.createObjectNode());
    }

    @Test
    @WithMockUser(username = "admin", roles = "ADMIN")
    void chatQueuesControl() throws Exception {
        mvc.perform(post("/api/chat").contentType(MediaType.APPLICATION_JSON).content("""
                        {"device_id":"ESP32-CHAT","text":"turn on relay 7"}
                        """))
                .andExpect(status().isOk())
                .andExpect(jsonPath("$.understood").value(true))
                .andExpect(jsonPath("$.action").value("control"))
                .andExpect(jsonPath("$.command.target").value("relay"))
                .andExpect(jsonPath("$.command.id").value(7))
                .andExpect(jsonPath("$.queued.ok").value(true));
    }

    @Test
    @WithMockUser(username = "admin", roles = "ADMIN")
    void chatReportsUnknown() throws Exception {
        mvc.perform(post("/api/chat").contentType(MediaType.APPLICATION_JSON).content("""
                        {"device_id":"ESP32-CHAT","text":"hello there friend"}
                        """))
                .andExpect(status().isOk())
                .andExpect(jsonPath("$.understood").value(false))
                .andExpect(jsonPath("$.action").value("unknown"));
    }

    @Test
    @WithMockUser(username = "admin", roles = "ADMIN")
    void chatRequiresDevice() throws Exception {
        mvc.perform(post("/api/chat").contentType(MediaType.APPLICATION_JSON).content("""
                        {"text":"status"}
                        """))
                .andExpect(status().isBadRequest());
    }
}
