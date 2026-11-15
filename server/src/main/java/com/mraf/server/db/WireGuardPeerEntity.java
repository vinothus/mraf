package com.mraf.server.db;

import jakarta.persistence.Column;
import jakarta.persistence.Entity;
import jakarta.persistence.Id;
import jakarta.persistence.Table;

import java.time.Instant;

/**
 * A WireGuard peer that maps one ESP32 controller to a stable overlay address.
 */
@Entity
@Table(name = "wireguard_peers")
public class WireGuardPeerEntity {
    @Id
    @Column(name = "device_id", length = 64)
    public String deviceId;

    @Column(name = "public_key", length = 64, nullable = false)
    public String publicKey;

    /** Client private key, retained so the config can be re-delivered. */
    @Column(name = "private_key", length = 64, nullable = false)
    public String privateKey;

    @Column(name = "preshared_key", length = 64)
    public String presharedKey = "";

    @Column(length = 32, nullable = false)
    public String address;

    @Column(name = "allowed_ips", length = 128)
    public String allowedIps = "";

    public boolean enabled = true;

    @Column(name = "created_at")
    public Instant createdAt = Instant.now();

    @Column(name = "last_handshake")
    public Instant lastHandshake;

    @Column(name = "delivered_at")
    public Instant deliveredAt;
}
