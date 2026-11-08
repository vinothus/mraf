import { createHash } from "node:crypto";

export const MAX_LABEL = 32;
export const MAX_ENDPOINT_ID = 64;

export function truncate(value, max = MAX_LABEL) {
    const text = String(value ?? "").trim();
    if (text.length <= max) return text;
    return text.slice(0, max).trimEnd();
}

export function sanitizePart(value) {
    const text = String(value ?? "")
        .replace(/[^A-Za-z0-9_-]+/g, "_")
        .replace(/^_+|_+$/g, "");
    return text || "device";
}

export function shortHash(value) {
    return createHash("sha1").update(String(value ?? "")).digest("hex").slice(0, 6);
}

export function endpointId(deviceId, suffix) {
    const base = sanitizePart(deviceId).slice(0, 40);
    const id = `mraf-${base}-${shortHash(deviceId)}-${sanitizePart(suffix)}`;
    return id.slice(0, MAX_ENDPOINT_ID);
}

export function labelFor(deviceName, channel) {
    const name = channel?.name || `Relay ${channel?.id ?? ""}`.trim();
    const zone = String(channel?.zone ?? "").trim();
    const prefix = String(deviceName ?? "").trim() || "MRAF";
    return truncate(zone ? `${prefix} - ${zone} ${name}` : `${prefix} - ${name}`);
}

export function pumpLabel(deviceName) {
    return truncate(`${String(deviceName ?? "").trim() || "MRAF"} - Pump`);
}

export function sensorLabel(deviceName, kind) {
    const suffix = kind === "tank" ? "Tank full" : kind === "sump" ? "Sump dry" : kind;
    return truncate(`${String(deviceName ?? "").trim() || "MRAF"} - ${suffix}`);
}

export function relayDeviceKind(equipment) {
    return equipment === "light" ? "light" : "plug";
}

export function relayControl(channel, on) {
    const command = {
        target: "relay",
        id: channel.id,
        mode: on ? "on" : "off",
    };
    if (channel.pulse_mode) {
        command.pulse_ms = on
            ? Number(channel.pulse_on_ms) || 600
            : Number(channel.pulse_off_ms) || 600;
    }
    return command;
}

export function pumpControl(on) {
    return { target: "pump", mode: on ? "on" : "off" };
}

export function channelsOf(config) {
    const channels = Array.isArray(config?.channels) ? config.channels : [];
    if (channels.length) return channels;
    return Array.from({ length: 12 }, (_, index) => ({
        id: index + 1,
        name: `Relay ${index + 1}`,
        zone: "",
        equipment: "other",
        pulse_mode: false,
        pulse_on_ms: 600,
        pulse_off_ms: 600,
    }));
}

export function snapshotState(data) {
    const relays = Array.isArray(data?.relays) ? data.relays : [];
    return {
        relays: Array.from({ length: 12 }, (_, index) => Boolean(relays[index])),
        pumpOn: Boolean(data?.pump?.on),
        tankFull: Boolean(data?.tank_full),
        sumpDry: Boolean(data?.sump_dry),
    };
}
