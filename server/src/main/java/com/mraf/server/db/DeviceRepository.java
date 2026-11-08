package com.mraf.server.db;

import org.springframework.data.jpa.repository.JpaRepository;

import java.util.Optional;

public interface DeviceRepository extends JpaRepository<DeviceEntity, String> {
    Optional<DeviceEntity> findByRemoteUser(String remoteUser);

    boolean existsByRemoteUserAndIdNot(String remoteUser, String id);
}
