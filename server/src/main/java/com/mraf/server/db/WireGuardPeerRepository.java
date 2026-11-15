package com.mraf.server.db;

import org.springframework.data.jpa.repository.JpaRepository;

import java.util.List;

public interface WireGuardPeerRepository extends JpaRepository<WireGuardPeerEntity, String> {
    List<WireGuardPeerEntity> findAllByOrderByAddressAsc();
}
