package com.mraf.server.db;

import org.springframework.data.jpa.repository.JpaRepository;

import java.util.List;

public interface DeviceRequestRepository extends JpaRepository<DeviceRequestEntity, String> {
    List<DeviceRequestEntity> findByStatusNotOrderByCreatedAtAsc(String status);

    List<DeviceRequestEntity> findByDeviceIdAndStatusNotOrderByCreatedAtAsc(String deviceId, String status);

    long countByDeviceIdAndStatus(String deviceId, String status);

    void deleteByDeviceId(String deviceId);

    void deleteByDeviceIdAndStatusIn(String deviceId, List<String> statuses);
}
