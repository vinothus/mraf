package com.mraf.server.config;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.mraf.server.coap.MrafCoapResource;
import com.mraf.server.service.RemoteStateService;
import org.eclipse.californium.core.CoapResource;
import org.eclipse.californium.core.CoapServer;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;

import java.util.ArrayList;
import java.util.List;

@Configuration
public class CoapConfig {
    @Bean(initMethod = "start", destroyMethod = "stop")
    CoapServer coapServer(
            ObjectMapper mapper,
            RemoteStateService state,
            @Value("${mraf.coap.port:5683}") int port,
            @Value("${mraf.coap.path:/mraf}") String path) {
        org.eclipse.californium.elements.config.Configuration coapConfig =
                org.eclipse.californium.elements.config.Configuration.createStandardWithoutFile();
        coapConfig.set(org.eclipse.californium.core.config.CoapConfig.MAX_MESSAGE_SIZE, 8192);
        coapConfig.set(org.eclipse.californium.core.config.CoapConfig.MAX_RESOURCE_BODY_SIZE, 8192);
        coapConfig.set(org.eclipse.californium.elements.config.UdpConfig.UDP_DATAGRAM_SIZE, 8192);
        CoapServer server = new CoapServer(coapConfig, port);

        List<String> segments = new ArrayList<>();
        for (String segment : path.split("/")) {
            if (!segment.isBlank()) {
                segments.add(segment);
            }
        }
        if (segments.isEmpty()) {
            segments.add("mraf");
        }

        CoapResource root = new MrafCoapResource(segments.get(segments.size() - 1), mapper, state);
        for (int i = segments.size() - 2; i >= 0; i--) {
            CoapResource parent = new CoapResource(segments.get(i));
            parent.add(root);
            root = parent;
        }
        server.add(root);
        return server;
    }
}
