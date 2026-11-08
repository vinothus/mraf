package com.mraf.server.coap;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.mraf.server.service.RemoteStateService;
import org.eclipse.californium.core.CoapResource;
import org.eclipse.californium.core.coap.CoAP.ResponseCode;
import org.eclipse.californium.core.coap.MediaTypeRegistry;
import org.eclipse.californium.core.server.resources.CoapExchange;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

public class MrafCoapResource extends CoapResource {
    private static final Logger log = LoggerFactory.getLogger(MrafCoapResource.class);

    private final ObjectMapper mapper;
    private final RemoteStateService state;

    public MrafCoapResource(String name, ObjectMapper mapper, RemoteStateService state) {
        super(name);
        this.mapper = mapper;
        this.state = state;
        getAttributes().setTitle("MRAF controller poll endpoint");
    }

    @Override
    public void handlePOST(CoapExchange exchange) {
        try {
            String body = exchange.getRequestText();
            JsonNode request = mapper.readTree(body);
            if (request == null || !request.isObject()) {
                exchange.respond(ResponseCode.BAD_REQUEST, "{\"error\":\"JSON object required\"}",
                        MediaTypeRegistry.APPLICATION_JSON);
                return;
            }
            if (!state.tokenMatches(request.path("token").asText(""))) {
                log.warn("Rejected CoAP poll with a bad token");
                exchange.respond(ResponseCode.UNAUTHORIZED, "{\"error\":\"bad token\"}",
                        MediaTypeRegistry.APPLICATION_JSON);
                return;
            }
            JsonNode response = state.handlePoll(request);
            exchange.respond(ResponseCode.CHANGED, mapper.writeValueAsString(response),
                    MediaTypeRegistry.APPLICATION_JSON);
        } catch (Exception e) {
            log.warn("CoAP poll failed", e);
            exchange.respond(ResponseCode.BAD_REQUEST, "{\"error\":\"invalid request\"}",
                    MediaTypeRegistry.APPLICATION_JSON);
        }
    }
}
