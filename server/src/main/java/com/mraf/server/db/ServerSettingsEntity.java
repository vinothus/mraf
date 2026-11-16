package com.mraf.server.db;

import jakarta.persistence.Column;
import jakarta.persistence.Entity;
import jakarta.persistence.Id;
import jakarta.persistence.Table;

/**
 * Server-wide UI settings. A single row (id = "branding") holds the dashboard
 * heading and subheading shown on the sign-in screen and in the app header.
 */
@Entity
@Table(name = "server_settings")
public class ServerSettingsEntity {
    @Id
    @Column(length = 32)
    public String id;

    @Column(length = 120)
    public String heading = "";

    @Column(length = 200)
    public String subheading = "";

    public ServerSettingsEntity() {
    }

    public ServerSettingsEntity(String id) {
        this.id = id;
    }
}
