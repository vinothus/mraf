package com.mraf.server.service;

/**
 * Lightweight application events published by {@link RemoteStateService} so the
 * WireGuard control plane can react without a direct dependency.
 */
public final class MrafEvents {
    private MrafEvents() {
    }

    /** Raised when an administrator confirms a controller. */
    public record DeviceConfirmed(String deviceId) {
    }

    /** Raised when a controller acknowledges a queued request. */
    public record RequestAcked(String deviceId, String requestType) {
    }
}
