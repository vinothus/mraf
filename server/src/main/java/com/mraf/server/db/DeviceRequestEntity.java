package com.mraf.server.db;

import jakarta.persistence.Column;
import jakarta.persistence.Entity;
import jakarta.persistence.Id;
import jakarta.persistence.Index;
import jakarta.persistence.Lob;
import jakarta.persistence.Table;

import java.time.Instant;

@Entity
@Table(name = "device_requests", indexes = {
        @Index(name = "idx_requests_device_status", columnList = "device_id,status,created_at")
})
public class DeviceRequestEntity {
    @Id
    @Column(length = 128)
    public String id;

    @Column(name = "device_id", length = 64, nullable = false)
    public String deviceId;

    @Column(length = 16)
    public String type;

    @Column(length = 16)
    public String status = "QUEUED";

    @Lob
    @Column(name = "payload_json", columnDefinition = "CLOB")
    public String payloadJson;

    public int attempts;

    @Column(name = "created_at")
    public Instant createdAt = Instant.now();

    @Column(name = "delivered_at")
    public Instant deliveredAt;

    @Column(name = "acked_at")
    public Instant ackedAt;

    @Column(length = 32)
    public String result;
}
