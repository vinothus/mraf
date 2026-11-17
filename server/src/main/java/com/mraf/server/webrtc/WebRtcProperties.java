package com.mraf.server.webrtc;

import org.springframework.boot.context.properties.ConfigurationProperties;
import org.springframework.stereotype.Component;

/**
 * Configuration for the WebRTC control plane. The Spring Boot server never
 * carries the WebRTC data: it only runs the STUN server used for ICE candidate
 * gathering and a small signalling registry so a browser and a controller can
 * find each other. Once the peer connection is up the JSON dashboard traffic
 * flows directly between the two peers (DTLS-encrypted, invisible to us).
 */
@Component
@ConfigurationProperties(prefix = "mraf.webrtc")
public class WebRtcProperties {
    /** Master switch for the STUN server and signalling registry. */
    private boolean enabled = true;

    /** Host name advertised to clients in the STUN URL (blank = use request host). */
    private String publicHost = "";

    /** Optional externally reachable base URL (blank = derive from the request). */
    private String publicUrl = "";

    private final Stun stun = new Stun();
    private final Signal signal = new Signal();
    private final Turn turn = new Turn();

    public static class Turn {
        /** Advertise a TURN relay alongside STUN. */
        private boolean enabled = false;
        /** Host advertised to clients (blank = use request host). */
        private String host = "";
        private int port = 3478;
        /** Optional full turn: URL; overrides host/port when set. */
        private String url = "";
        /** Long-term credential username. */
        private String username = "";
        /** Long-term credential password. */
        private String credential = "";

        public boolean isEnabled() {
            return enabled;
        }

        public void setEnabled(boolean enabled) {
            this.enabled = enabled;
        }

        public String getHost() {
            return host;
        }

        public void setHost(String host) {
            this.host = host;
        }

        public int getPort() {
            return port;
        }

        public void setPort(int port) {
            this.port = port;
        }

        public String getUrl() {
            return url;
        }

        public void setUrl(String url) {
            this.url = url;
        }

        public String getUsername() {
            return username;
        }

        public void setUsername(String username) {
            this.username = username;
        }

        public String getCredential() {
            return credential;
        }

        public void setCredential(String credential) {
            this.credential = credential;
        }
    }

    public static class Stun {
        private boolean enabled = true;
        private int port = 3478;

        public boolean isEnabled() {
            return enabled;
        }

        public void setEnabled(boolean enabled) {
            this.enabled = enabled;
        }

        public int getPort() {
            return port;
        }

        public void setPort(int port) {
            this.port = port;
        }
    }

    public static class Signal {
        /** How long an idle session is kept before the janitor removes it. */
        private int ttlSeconds = 180;
        /** Upper bound on queued signalling messages per session. */
        private int maxQueue = 256;
        /** Suggested browser polling interval for signalling (ms). */
        private int pollMs = 400;
        /** Largest accepted SDP/candidate payload. */
        private int maxPayloadBytes = 16384;

        public int getTtlSeconds() {
            return ttlSeconds;
        }

        public void setTtlSeconds(int ttlSeconds) {
            this.ttlSeconds = ttlSeconds;
        }

        public int getMaxQueue() {
            return maxQueue;
        }

        public void setMaxQueue(int maxQueue) {
            this.maxQueue = maxQueue;
        }

        public int getPollMs() {
            return pollMs;
        }

        public void setPollMs(int pollMs) {
            this.pollMs = pollMs;
        }

        public int getMaxPayloadBytes() {
            return maxPayloadBytes;
        }

        public void setMaxPayloadBytes(int maxPayloadBytes) {
            this.maxPayloadBytes = maxPayloadBytes;
        }
    }

    public boolean isEnabled() {
        return enabled;
    }

    public void setEnabled(boolean enabled) {
        this.enabled = enabled;
    }

    public String getPublicHost() {
        return publicHost;
    }

    public void setPublicHost(String publicHost) {
        this.publicHost = publicHost;
    }

    public String getPublicUrl() {
        return publicUrl;
    }

    public void setPublicUrl(String publicUrl) {
        this.publicUrl = publicUrl;
    }

    public Stun getStun() {
        return stun;
    }

    public Turn getTurn() {
        return turn;
    }

    public Signal getSignal() {
        return signal;
    }
}
