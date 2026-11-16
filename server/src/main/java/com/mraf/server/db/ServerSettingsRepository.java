package com.mraf.server.db;

import org.springframework.data.jpa.repository.JpaRepository;

public interface ServerSettingsRepository extends JpaRepository<ServerSettingsEntity, String> {
}
