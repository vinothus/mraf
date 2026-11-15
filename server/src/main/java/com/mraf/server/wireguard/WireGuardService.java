package com.mraf.server.wireguard;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.mraf.server.db.WireGuardPeerEntity;
import com.mraf.server.db.WireGuardPeerRepository;
import com.mraf.server.service.MrafEvents;
import com.mraf.server.service.RemoteStateService;
import jakarta.annotation.PostConstruct;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.context.event.EventListener;
import org.springframework.stereotype.Service;
import org.springframework.transaction.annotation.Transactional;

import java.io.IOException;
import java.net.InetAddress;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;

/**
 * Server side WireGuard control plane. Allocates stable overlay addresses,
 * keeps a registry of peers, renders {@code wg-quick} configuration and,
 * optionally, applies it to the host kernel.
 */
@Service
public class WireGuardService {
    private static final Logger log = LoggerFactory.getLogger(WireGuardService.class);

    private final WireGuardProperties props;
    private final WireGuardKeyService keys;
    private final WireGuardPeerRepository peerRepo;
    private final RemoteStateService state;
    private final ObjectMapper mapper;

    private String serverPrivateKey;
    private String serverPublicKey;

    public WireGuardService(WireGuardProperties props, WireGuardKeyService keys,
                            WireGuardPeerRepository peerRepo, RemoteStateService state,
                            ObjectMapper mapper) {
        this.props = props;
        this.keys = keys;
        this.peerRepo = peerRepo;
        this.state = state;
        this.mapper = mapper;
    }

    @PostConstruct
    void init() {
        if (!props.isEnabled()) {
            return;
        }
        try {
            Path dir = Path.of(props.getConfigDir());
            Files.createDirectories(dir);
            Path keyFile = dir.resolve("server.key");
            if (Files.exists(keyFile)) {
                serverPrivateKey = Files.readString(keyFile).trim();
            } else {
                serverPrivateKey = keys.generate().privateKey();
                Files.writeString(keyFile, serverPrivateKey + System.lineSeparator());
                log.info("Generated new WireGuard server key at {}", keyFile);
            }
            serverPublicKey = keys.publicKeyFromPrivate(serverPrivateKey);
            log.info("WireGuard control plane ready on {} (server {})",
                    props.getInterfaceName(), props.getServerAddress());
        } catch (Exception e) {
            throw new IllegalStateException("could not initialise WireGuard server identity", e);
        }
    }

    // ------------------------------------------------------------------ peers
    @Transactional
    public synchronized WireGuardPeerEntity ensurePeer(String deviceId) {        requireEnabled();
        if (deviceId == null || deviceId.isBlank()) {
            throw new IllegalArgumentException("device id is required");
        }
        WireGuardPeerEntity existing = peerRepo.findById(deviceId).orElse(null);
        if (existing != null) {
            return existing;
        }
        WireGuardKeyService.KeyPairStrings kp = keys.generate();
        WireGuardPeerEntity peer = new WireGuardPeerEntity();
        peer.deviceId = deviceId;
        peer.privateKey = kp.privateKey();
        peer.publicKey = kp.publicKey();
        peer.presharedKey = keys.generatePresharedKey();
        peer.address = nextAddress() + "/32";
        peer.allowedIps = peer.address;
        peer.createdAt = Instant.now();
        peerRepo.save(peer);
        log.info("Provisioned WireGuard peer {} at {}", deviceId, peer.address);
        return peer;
    }

    public synchronized WireGuardPeerEntity getPeer(String deviceId) {
        return peerRepo.findById(deviceId).orElse(null);
    }

    public synchronized List<WireGuardPeerEntity> listPeers() {
        return peerRepo.findAllByOrderByAddressAsc();
    }

    @Transactional
    public synchronized void removePeer(String deviceId) {
        peerRepo.deleteById(deviceId);
    }

    @Transactional
    public synchronized void markDelivered(String deviceId) {
        WireGuardPeerEntity peer = peerRepo.findById(deviceId).orElse(null);
        if (peer != null) {
            peer.deliveredAt = Instant.now();
            peerRepo.save(peer);
        }
    }

    @Transactional
    public synchronized void markHandshake(String deviceId) {
        WireGuardPeerEntity peer = peerRepo.findById(deviceId).orElse(null);
        if (peer != null) {
            peer.lastHandshake = Instant.now();
            peerRepo.save(peer);
        }
    }

    // ------------------------------------------------------------------ events
    /** Auto-provision a tunnel as soon as an administrator confirms a device. */
    @EventListener
    public void onDeviceConfirmed(MrafEvents.DeviceConfirmed event) {
        if (!props.isEnabled() || !props.isAutoProvision()) {
            return;
        }
        try {
            ensurePeer(event.deviceId());
            state.queueWireGuard(event.deviceId(), clientConfig(event.deviceId(), null));
            log.info("Queued WireGuard configuration for newly confirmed device {}",
                    event.deviceId());
        } catch (Exception e) {
            log.warn("Could not auto-provision WireGuard for {}", event.deviceId(), e);
        }
    }

    /** A device acked its WireGuard config, which we treat as a handshake proxy. */
    @EventListener
    public void onRequestAcked(MrafEvents.RequestAcked event) {
        if ("WIREGUARD".equals(event.requestType())) {
            markDelivered(event.deviceId());
            markHandshake(event.deviceId());
        }
    }

    // ---------------------------------------------------------------- rendering
    public synchronized ObjectNode clientConfig(String deviceId, String endpoint) {
        WireGuardPeerEntity peer = ensurePeer(deviceId);
        ObjectNode out = mapper.createObjectNode();
        out.put("interface", props.getInterfaceName());
        out.put("address", peer.address);
        out.put("private_key", peer.privateKey);
        if (!props.getDns().isBlank()) {
            out.put("dns", props.getDns());
        }
        ObjectNode remote = out.putObject("peer");
        remote.put("public_key", serverPublicKey);
        remote.put("endpoint", resolveEndpoint(endpoint));
        remote.put("allowed_ips", props.getAllowedIps());
        remote.put("persistent_keepalive", props.getPersistentKeepalive());
        remote.put("preshared_key", peer.presharedKey);
        return out;
    }

    public synchronized String renderClientConfig(String deviceId, String endpoint) {
        WireGuardPeerEntity peer = ensurePeer(deviceId);
        StringBuilder sb = new StringBuilder();
        sb.append("[Interface]").append('\n');
        sb.append("PrivateKey = ").append(peer.privateKey).append('\n');
        sb.append("Address = ").append(peer.address).append('\n');
        if (!props.getDns().isBlank()) {
            sb.append("DNS = ").append(props.getDns()).append('\n');
        }
        sb.append('\n');
        sb.append("[Peer]").append('\n');
        sb.append("PublicKey = ").append(serverPublicKey).append('\n');
        if (!peer.presharedKey.isBlank()) {
            sb.append("PresharedKey = ").append(peer.presharedKey).append('\n');
        }
        sb.append("Endpoint = ").append(resolveEndpoint(endpoint)).append('\n');
        sb.append("AllowedIPs = ").append(props.getAllowedIps()).append('\n');
        sb.append("PersistentKeepalive = ").append(props.getPersistentKeepalive()).append('\n');
        return sb.toString();
    }

    public synchronized String renderServerConfig() {
        StringBuilder sb = new StringBuilder();
        sb.append("[Interface]").append('\n');
        sb.append("Address = ").append(serverCidr()).append('\n');
        sb.append("ListenPort = ").append(props.getListenPort()).append('\n');
        sb.append("PrivateKey = ").append(serverPrivateKey).append('\n');
        for (WireGuardPeerEntity peer : listPeers()) {
            sb.append('\n');
            sb.append("# ").append(peer.deviceId).append('\n');
            sb.append("[Peer]").append('\n');
            sb.append("PublicKey = ").append(peer.publicKey).append('\n');
            if (!peer.presharedKey.isBlank()) {
                sb.append("PresharedKey = ").append(peer.presharedKey).append('\n');
            }
            sb.append("AllowedIPs = ").append(peer.allowedIps).append('\n');
        }
        return sb.toString();
    }

    // ------------------------------------------------------------------- status
    public synchronized ObjectNode status() {
        ObjectNode out = mapper.createObjectNode();
        out.put("enabled", props.isEnabled());
        out.put("interface", props.getInterfaceName());
        out.put("subnet", props.getSubnet());
        out.put("serverAddress", props.getServerAddress());
        out.put("listenPort", props.getListenPort());
        out.put("endpoint", props.getEndpoint().isBlank() ? "" : props.getEndpoint());
        out.put("persistentKeepalive", props.getPersistentKeepalive());
        out.put("manageKernel", props.isManageKernel());
        out.put("serverPublicKey", serverPublicKey == null ? "" : serverPublicKey);
        ArrayNode peers = out.putArray("peers");
        int online = 0;
        for (WireGuardPeerEntity peer : listPeers()) {
            ObjectNode n = peers.addObject();
            n.put("deviceId", peer.deviceId);
            n.put("address", peer.address);
            n.put("publicKey", peer.publicKey);
            n.put("enabled", peer.enabled);
            n.put("createdAt", peer.createdAt == null ? "" : peer.createdAt.toString());
            n.put("deliveredAt", peer.deliveredAt == null ? "" : peer.deliveredAt.toString());
            n.put("lastHandshake", peer.lastHandshake == null ? "" : peer.lastHandshake.toString());
            boolean up = peer.lastHandshake != null
                    && Instant.now().getEpochSecond() - peer.lastHandshake.getEpochSecond() <= 180;
            n.put("online", up);
            if (up) {
                online++;
            }
        }
        out.put("peerCount", peers.size());
        out.put("onlinePeers", online);
        return out;
    }

    public String getServerPublicKey() {
        return serverPublicKey;
    }

    // -------------------------------------------------------------- kernel apply
    public synchronized ObjectNode applyKernel() {
        ObjectNode out = mapper.createObjectNode();
        String conf = renderServerConfig();
        Path dir = Path.of(props.getConfigDir());
        Path file = dir.resolve(props.getInterfaceName() + ".conf");
        try {
            Files.createDirectories(dir);
            Files.writeString(file, conf);
            out.put("configPath", file.toString());
        } catch (IOException e) {
            throw new IllegalStateException("could not write config: " + e.getMessage(), e);
        }
        if (!props.isManageKernel()) {
            out.put("applied", false);
            out.put("message", "kernel management disabled; configuration rendered only");
            return out;
        }
        String down = run("wg-quick", "down", props.getInterfaceName());
        String up = run("wg-quick", "up", file.toString());
        out.put("downOutput", down);
        out.put("upOutput", up);
        boolean applied = up != null && !up.toLowerCase().contains("not found")
                && !up.toLowerCase().contains("permission denied");
        out.put("applied", applied);
        out.put("message", applied ? "wg-quick applied " + props.getInterfaceName()
                : "wg-quick not available on this host");
        return out;
    }

    private String run(String... command) {
        try {
            Process process = new ProcessBuilder(command)
                    .redirectErrorStream(true)
                    .start();
            String output = new String(process.getInputStream().readAllBytes());
            process.waitFor();
            return output.trim();
        } catch (IOException e) {
            return "not found: " + e.getMessage();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return "interrupted";
        }
    }

    // ---------------------------------------------------------------- helpers
    private void requireEnabled() {
        if (!props.isEnabled()) {
            throw new IllegalStateException("WireGuard support is disabled");
        }
    }

    private String resolveEndpoint(String override) {
        String host = override == null ? "" : override.trim();
        if (host.isBlank()) {
            host = props.getEndpoint();
        }
        if (host.isBlank()) {
            host = "127.0.0.1";
        }
        if (host.contains(":")) {
            return host;
        }
        return host + ":" + props.getListenPort();
    }

    private String serverCidr() {
        int prefix = prefixLength();
        return props.getServerAddress() + "/" + prefix;
    }

    private int prefixLength() {
        String subnet = props.getSubnet();
        int slash = subnet.indexOf('/');
        if (slash < 0) {
            return 24;
        }
        try {
            return Integer.parseInt(subnet.substring(slash + 1));
        } catch (NumberFormatException e) {
            return 24;
        }
    }

    private synchronized String nextAddress() {
        String subnet = props.getSubnet();
        int slash = subnet.indexOf('/');
        String base = slash < 0 ? subnet : subnet.substring(0, slash);
        long network = ipToLong(base) & mask(prefixLength());
        long broadcast = network | ((~mask(prefixLength())) & 0xFFFFFFFFL);
        long server = ipToLong(props.getServerAddress());
        List<Long> used = new ArrayList<>();
        for (WireGuardPeerEntity peer : listPeers()) {
            String addr = peer.address == null ? "" : peer.address;
            int s = addr.indexOf('/');
            used.add(ipToLong(s < 0 ? addr : addr.substring(0, s)));
        }
        for (long candidate = network + 1; candidate < broadcast; candidate++) {
            if (candidate == server || used.contains(candidate)) {
                continue;
            }
            return longToIp(candidate);
        }
        throw new IllegalStateException("WireGuard subnet " + props.getSubnet() + " is exhausted");
    }

    private static long mask(int prefix) {
        if (prefix <= 0) {
            return 0;
        }
        if (prefix >= 32) {
            return 0xFFFFFFFFL;
        }
        return (0xFFFFFFFFL << (32 - prefix)) & 0xFFFFFFFFL;
    }

    private static long ipToLong(String ip) {
        try {
            byte[] bytes = InetAddress.getByName(ip).getAddress();
            long value = 0;
            for (byte b : bytes) {
                value = (value << 8) | (b & 0xFF);
            }
            return value & 0xFFFFFFFFL;
        } catch (Exception e) {
            throw new IllegalArgumentException("invalid IPv4 address: " + ip, e);
        }
    }

    private static String longToIp(long value) {
        return ((value >> 24) & 0xFF) + "." + ((value >> 16) & 0xFF) + "."
                + ((value >> 8) & 0xFF) + "." + (value & 0xFF);
    }
}
