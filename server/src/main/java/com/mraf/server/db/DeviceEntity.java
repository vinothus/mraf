package com.mraf.server.db;

import jakarta.persistence.Column;
import jakarta.persistence.Entity;
import jakarta.persistence.Id;
import jakarta.persistence.Lob;
import jakarta.persistence.Table;

import java.time.Instant;

@Entity
@Table(name = "devices")
public class DeviceEntity {
    @Id
    @Column(length = 64)
    public String id;

    @Column(length = 64)
    public String name = "";

    @Column(length = 32)
    public String firmware = "";

    @Column(length = 16)
    public String profile = "agri";

    public boolean confirmed;
    public boolean rejected;

    @Column(name = "remote_user", length = 64)
    public String remoteUser = "";

    @Column(name = "password_hash", length = 100)
    public String passwordHash = "";

    @Column(name = "poll_interval_sec")
    public int pollIntervalSec = 60;

    @Column(name = "first_seen")
    public Instant firstSeen = Instant.now();

    @Column(name = "last_poll_at")
    public Instant lastPollAt;

    @Column(name = "poll_count")
    public long pollCount;

    @Column(name = "request_seq")
    public long requestSeq;

    @Lob
    @Column(name = "config_json", columnDefinition = "CLOB")
    public String configJson;

    @Lob
    @Column(name = "last_data_json", columnDefinition = "CLOB")
    public String lastDataJson;
}
