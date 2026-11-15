package com.mraf.server.mcp;

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
        "spring.datasource.url=jdbc:h2:mem:mraf-mcp;DB_CLOSE_DELAY=-1",
        "spring.jpa.hibernate.ddl-auto=create-drop",
        "mraf.coap.port=0",
        "mraf.token=secret",
        "mraf.wireguard.config-dir=target/test-mcp-wg"
})
@AutoConfigureMockMvc
class McpControllerTest {
    private static final String DEVICE = "ESP32-MCP";

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
        auth.put("user", "mcpuser");
        auth.put("pass", "pass1234");
        state.handlePoll(poll);
        state.confirmDevice(DEVICE, mapper.createObjectNode());
    }

    @Test
    @WithMockUser(username = "admin", roles = "ADMIN")
    void initializeHandshake() throws Exception {
        mvc.perform(post("/mcp").contentType(MediaType.APPLICATION_JSON).content("""
                        {"jsonrpc":"2.0","id":1,"method":"initialize",
                         "params":{"protocolVersion":"2025-06-18"}}
                        """))
                .andExpect(status().isOk())
                .andExpect(jsonPath("$.result.serverInfo.name").value("mraf-server"))
                .andExpect(jsonPath("$.result.capabilities.tools").exists());
    }

    @Test
    @WithMockUser(username = "admin", roles = "ADMIN")
    void listsTools() throws Exception {
        mvc.perform(post("/mcp").contentType(MediaType.APPLICATION_JSON).content("""
                        {"jsonrpc":"2.0","id":2,"method":"tools/list"}
                        """))
                .andExpect(status().isOk())
                .andExpect(jsonPath("$.result.tools[?(@.name=='control_relay')]").exists())
                .andExpect(jsonPath("$.result.tools[?(@.name=='wireguard_provision')]").exists())
                .andExpect(jsonPath("$.result.tools[?(@.name=='chat')]").exists());
    }

    @Test
    @WithMockUser(username = "admin", roles = "ADMIN")
    void callsControlRelayTool() throws Exception {
        mvc.perform(post("/mcp").contentType(MediaType.APPLICATION_JSON).content("""
                        {"jsonrpc":"2.0","id":3,"method":"tools/call",
                         "params":{"name":"control_relay",
                           "arguments":{"device_id":"ESP32-MCP","relay_id":3,"mode":"on"}}}
                        """))
                .andExpect(status().isOk())
                .andExpect(jsonPath("$.result.isError").value(false));
        // Optimistic mode update is visible in the snapshot.
        org.junit.jupiter.api.Assertions.assertEquals("on",
                state.snapshot(DEVICE).path("config").path("channels").get(2).path("mode").asText());
    }

    @Test
    @WithMockUser(username = "admin", roles = "ADMIN")
    void chatToolUnderstandsPhrase() throws Exception {
        mvc.perform(post("/mcp").contentType(MediaType.APPLICATION_JSON).content("""
                        {"jsonrpc":"2.0","id":4,"method":"tools/call",
                         "params":{"name":"chat",
                           "arguments":{"device_id":"ESP32-MCP","text":"turn on relay 4"}}}
                        """))
                .andExpect(status().isOk())
                .andExpect(jsonPath("$.result.isError").value(false))
                .andExpect(jsonPath("$.result.content[0].text").value(
                        org.hamcrest.Matchers.containsString("relay 4")));
    }

    @Test
    @WithMockUser(username = "admin", roles = "ADMIN")
    void unknownToolIsErrorResult() throws Exception {
        mvc.perform(post("/mcp").contentType(MediaType.APPLICATION_JSON).content("""
                        {"jsonrpc":"2.0","id":5,"method":"tools/call",
                         "params":{"name":"nope","arguments":{}}}
                        """))
                .andExpect(status().isOk())
                .andExpect(jsonPath("$.result.isError").value(true));
    }

    @Test
    void requiresAuthentication() throws Exception {
        mvc.perform(post("/mcp").contentType(MediaType.APPLICATION_JSON).content("""
                        {"jsonrpc":"2.0","id":6,"method":"tools/list"}
                        """))
                .andExpect(status().isUnauthorized());
    }
}
