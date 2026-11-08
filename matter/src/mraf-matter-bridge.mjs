#!/usr/bin/env node
/**
 * MRAF Matter bridge.
 *
 * Exposes the relays, pump and float sensors managed by the MRAF Spring Boot
 * server as Matter OnOff / Contact Sensor endpoints. The bridge talks to the
 * server over its REST API; the ESP32 firmware and CoAP protocol are unchanged.
 */

const [nodeMajor, nodeMinor] = process.versions.node.split(".").map(Number);
if (!Number.isFinite(nodeMajor) || nodeMajor < 20 || (nodeMajor === 20 && nodeMinor < 19)) {
    console.error(
        `MRAF Matter bridge needs Node.js 20.19 or newer; this is Node ${process.versions.node}.\n` +
            "Install Node 20.19+/22 and run `npm install` again in matter/.",
    );
    process.exit(1);
}

const { Endpoint, Environment, ServerNode, VendorId } = await import("@matter/main");
const { BridgedDeviceBasicInformationServer } = await import(
    "@matter/main/behaviors/bridged-device-basic-information"
);
const { OnOffLightDevice } = await import("@matter/main/devices/on-off-light");
const { OnOffPlugInUnitDevice } = await import("@matter/main/devices/on-off-plug-in-unit");
const { ContactSensorDevice } = await import("@matter/main/devices/contact-sensor");
const { AggregatorEndpoint } = await import("@matter/main/endpoints/aggregator");
const qrcode = (await import("qrcode-terminal")).default;

const {
    channelsOf,
    endpointId,
    labelFor,
    pumpControl,
    pumpLabel,
    relayControl,
    relayDeviceKind,
    sensorLabel,
    snapshotState,
    truncate,
} = await import("./mapping.mjs");

const env = process.env;
const SERVER_URL = String(env.MRAF_SERVER_URL || "http://127.0.0.1:8080").replace(/\/+$/, "");
const SERVER_USER = env.MRAF_SERVER_USER || "admin";
const SERVER_PASSWORD = env.MRAF_SERVER_PASSWORD || "mraf-demo";
const DEVICE_FILTER = String(env.MRAF_MATTER_DEVICE || "").trim();
const POLL_MS = Math.max(1000, Number(env.MRAF_MATTER_POLL_MS || 5000));
const CONFIG_MS = Math.max(POLL_MS, Number(env.MRAF_MATTER_CONFIG_MS || 60000));
const COMMAND_GRACE_MS = Math.max(0, Number(env.MRAF_MATTER_COMMAND_GRACE_MS || 8000));
const MATTER_PORT = Number(env.MRAF_MATTER_PORT || 5540);
const PASSCODE = Number(env.MRAF_MATTER_PASSCODE || 20202021);
const DISCRIMINATOR = Number(env.MRAF_MATTER_DISCRIMINATOR || 3840);
const VENDOR_ID = Number(env.MRAF_MATTER_VENDOR_ID || 0xfff1);
const PRODUCT_ID = Number(env.MRAF_MATTER_PRODUCT_ID || 0x8001);
const NODE_ID = env.MRAF_MATTER_NODE_ID || "mraf-bridge";
const STORAGE_PATH = env.MRAF_MATTER_STORAGE || "data/matter";

if (!(PASSCODE >= 1 && PASSCODE <= 99999998)) {
    console.error(`MRAF_MATTER_PASSCODE must be 1..99999998 (got ${PASSCODE})`);
    process.exit(1);
}
if (!(DISCRIMINATOR >= 0 && DISCRIMINATOR <= 4095)) {
    console.error(`MRAF_MATTER_DISCRIMINATOR must be 0..4095 (got ${DISCRIMINATOR})`);
    process.exit(1);
}

Environment.default.vars.set("storage.path", STORAGE_PATH);
if (env.MRAF_MATTER_LOG_LEVEL) {
    Environment.default.vars.set("log.level", env.MRAF_MATTER_LOG_LEVEL);
}

const AUTH = `Basic ${Buffer.from(`${SERVER_USER}:${SERVER_PASSWORD}`).toString("base64")}`;

function log(message) {
    console.log(`[matter] ${message}`);
}

async function api(path, options = {}) {
    const response = await fetch(`${SERVER_URL}${path}`, {
        ...options,
        headers: {
            Authorization: AUTH,
            ...(options.headers || {}),
        },
    });
    if (!response.ok) {
        throw new Error(`${options.method || "GET"} ${path} -> HTTP ${response.status}`);
    }
    return response.json();
}

async function sendControl(deviceId, command) {
    await api(`/api/devices/${encodeURIComponent(deviceId)}/control`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(command),
    });
}

const server = await ServerNode.create({
    id: NODE_ID,
    network: { port: MATTER_PORT },
    commissioning: { passcode: PASSCODE, discriminator: DISCRIMINATOR },
    productDescription: {
        name: "MRAF Matter Bridge",
        deviceType: AggregatorEndpoint.deviceType,
    },
    basicInformation: {
        vendorName: "MRAF",
        vendorId: VendorId(VENDOR_ID),
        nodeLabel: "MRAF Farm Bridge",
        productName: "MRAF Matter Bridge",
        productLabel: "MRAF Bridge",
        productId: PRODUCT_ID,
        serialNumber: `${NODE_ID}-serial`,
        uniqueId: `${NODE_ID}-uid`,
    },
});

const aggregator = new Endpoint(AggregatorEndpoint, { id: "aggregator" });
await server.add(aggregator);
await server.start();

try {
    const codes = server.state.commissioning.pairingCodes;
    if (codes?.qrPairingCode) {
        console.log("");
        log("Matter bridge is ready to commission");
        console.log(`  QR payload:     ${codes.qrPairingCode}`);
        console.log(`  Manual code:    ${codes.manualPairingCode}`);
        console.log("");
        qrcode.generate(codes.qrPairingCode, { small: true });
        console.log("");
    } else {
        log("Matter bridge is already commissioned (or no pairing code is available)");
    }
} catch (error) {
    log(`Could not read pairing codes: ${error.message}`);
}

const records = new Map();
let syncDepth = 0;

async function withSync(fn) {
    syncDepth++;
    try {
        return await fn();
    } finally {
        syncDepth--;
    }
}

function createRecord(id) {
    return {
        id,
        name: id,
        endpoints: new Map(),
        config: null,
        configAt: 0,
        online: false,
        reachable: null,
        lastState: null,
    };
}

async function setNodeLabel(endpoint, label) {
    try {
        if (endpoint.state.bridgedDeviceBasicInformation.nodeLabel === label) return;
        await withSync(() =>
            endpoint.set({ bridgedDeviceBasicInformation: { nodeLabel: label } }),
        );
    } catch (error) {
        log(`label update failed: ${error.message}`);
    }
}

async function setReachable(record, online) {
    if (record.reachable === online) return;
    record.reachable = online;
    for (const entry of record.endpoints.values()) {
        try {
            await withSync(() =>
                entry.endpoint.set({
                    bridgedDeviceBasicInformation: { reachable: online },
                }),
            );
        } catch (error) {
            log(`reachable update failed for ${record.id}/${entry.key}: ${error.message}`);
        }
    }
}

async function addRelayEndpoint(record, channel, name) {
    const key = `r${channel.id}`;
    const existing = record.endpoints.get(key);
    const label = labelFor(name, channel);
    const deviceKind = relayDeviceKind(channel.equipment);

    if (existing) {
        existing.channel = channel;
        if (existing.deviceKind !== deviceKind) {
            log(
                `${record.id}/${key} changed equipment to ${channel.equipment}; ` +
                    "restart the bridge to recreate the Matter device type",
            );
        }
        await setNodeLabel(existing.endpoint, label);
        return;
    }

    const Device = deviceKind === "light" ? OnOffLightDevice : OnOffPlugInUnitDevice;
    const endpoint = new Endpoint(Device.with(BridgedDeviceBasicInformationServer), {
        id: endpointId(record.id, key),
        bridgedDeviceBasicInformation: {
            nodeLabel: label,
            productName: label,
            productLabel: truncate(channel.equipment || "relay"),
            serialNumber: endpointId(record.id, key),
            reachable: record.online,
        },
        onOff: { onOff: false },
    });
    await aggregator.add(endpoint);

    const entry = {
        key,
        kind: "relay",
        deviceKind,
        channel,
        endpoint,
        pendingUntil: 0,
        lastValue: false,
    };
    endpoint.events.onOff.onOff$Changed.on(value => {
        void onRelayCommand(record, entry, Boolean(value));
    });
    record.endpoints.set(key, entry);
    log(`${record.id}: exposed relay ${channel.id} (${channel.equipment || "other"}) as ${deviceKind}`);
}

async function addPumpEndpoint(record, name) {
    if (record.endpoints.has("pump")) {
        await setNodeLabel(record.endpoints.get("pump").endpoint, pumpLabel(name));
        return;
    }
    const label = pumpLabel(name);
    const endpoint = new Endpoint(OnOffPlugInUnitDevice.with(BridgedDeviceBasicInformationServer), {
        id: endpointId(record.id, "pump"),
        bridgedDeviceBasicInformation: {
            nodeLabel: label,
            productName: label,
            productLabel: "pump",
            serialNumber: endpointId(record.id, "pump"),
            reachable: record.online,
        },
        onOff: { onOff: false },
    });
    await aggregator.add(endpoint);
    const entry = {
        key: "pump",
        kind: "pump",
        endpoint,
        pendingUntil: 0,
        lastValue: false,
    };
    endpoint.events.onOff.onOff$Changed.on(value => {
        void onPumpCommand(record, entry, Boolean(value));
    });
    record.endpoints.set("pump", entry);
    log(`${record.id}: exposed pump`);
}

async function addSensorEndpoint(record, key, name) {
    const label = sensorLabel(name, key);
    const existing = record.endpoints.get(key);
    if (existing) {
        await setNodeLabel(existing.endpoint, label);
        return;
    }
    const endpoint = new Endpoint(ContactSensorDevice.with(BridgedDeviceBasicInformationServer), {
        id: endpointId(record.id, key),
        bridgedDeviceBasicInformation: {
            nodeLabel: label,
            productName: label,
            productLabel: key === "tank" ? "tank full" : "sump dry",
            serialNumber: endpointId(record.id, key),
            reachable: record.online,
        },
        booleanState: { stateValue: false },
    });
    await aggregator.add(endpoint);
    record.endpoints.set(key, {
        key,
        kind: "sensor",
        endpoint,
        pendingUntil: 0,
        lastValue: false,
    });
    log(`${record.id}: exposed ${key} sensor`);
}

async function ensureEndpoints(record, summary, config) {
    const name = summary.name || record.id;
    record.name = name;
    for (const channel of channelsOf(config)) {
        await addRelayEndpoint(record, channel, name);
    }
    await addPumpEndpoint(record, name);
    await addSensorEndpoint(record, "tank", name);
    await addSensorEndpoint(record, "sump", name);
}

async function setOnOff(entry, value) {
    if (entry.lastValue === value && entry.endpoint.state.onOff.onOff === value) return;
    entry.lastValue = value;
    if (entry.endpoint.state.onOff.onOff === value) return;
    await withSync(() => entry.endpoint.set({ onOff: { onOff: value } }));
}

async function setBoolean(entry, value) {
    if (entry.lastValue === value && entry.endpoint.state.booleanState.stateValue === value) return;
    entry.lastValue = value;
    if (entry.endpoint.state.booleanState.stateValue === value) return;
    await withSync(() => entry.endpoint.set({ booleanState: { stateValue: value } }));
}

async function applyState(record, summary) {
    const online = Boolean(summary.online);
    await setReachable(record, online);
    if (!online) return;

    const state = snapshotState(summary.lastData);
    const now = Date.now();

    for (const channel of channelsOf(record.config)) {
        const entry = record.endpoints.get(`r${channel.id}`);
        if (!entry || entry.pendingUntil > now) continue;
        try {
            await setOnOff(entry, state.relays[channel.id - 1]);
        } catch (error) {
            log(`state update failed for ${record.id}/r${channel.id}: ${error.message}`);
        }
    }

    const pump = record.endpoints.get("pump");
    if (pump && pump.pendingUntil <= now) {
        try {
            await setOnOff(pump, state.pumpOn);
        } catch (error) {
            log(`pump state update failed for ${record.id}: ${error.message}`);
        }
    }

    for (const [key, value] of [
        ["tank", state.tankFull],
        ["sump", state.sumpDry],
    ]) {
        const entry = record.endpoints.get(key);
        if (!entry) continue;
        try {
            await setBoolean(entry, value);
        } catch (error) {
            log(`${key} state update failed for ${record.id}: ${error.message}`);
        }
    }
    record.lastState = state;
}

async function onRelayCommand(record, entry, value) {
    if (syncDepth > 0) return;
    entry.pendingUntil = Date.now() + COMMAND_GRACE_MS;
    try {
        await sendControl(record.id, relayControl(entry.channel, value));
        log(`${record.id}: relay ${entry.channel.id} -> ${value ? "on" : "off"}`);
    } catch (error) {
        log(`relay ${entry.channel.id} command failed: ${error.message}`);
        await setOnOff(entry, !value);
    }
}

async function onPumpCommand(record, entry, value) {
    if (syncDepth > 0) return;
    entry.pendingUntil = Date.now() + COMMAND_GRACE_MS;
    try {
        await sendControl(record.id, pumpControl(value));
        log(`${record.id}: pump -> ${value ? "on" : "off"}`);
    } catch (error) {
        log(`pump command failed: ${error.message}`);
        await setOnOff(entry, !value);
    }
}

let polling = false;

async function poll() {
    if (polling) return;
    polling = true;
    try {
        const overview = await api("/api/devices");
        const devices = (overview.devices || []).filter(
            device =>
                device.confirmed &&
                !device.rejected &&
                (!DEVICE_FILTER || device.id === DEVICE_FILTER),
        );
        const seen = new Set();

        for (const summary of devices) {
            seen.add(summary.id);
            let record = records.get(summary.id);
            if (!record) {
                record = createRecord(summary.id);
                records.set(summary.id, record);
                log(`discovered confirmed device ${summary.id}`);
            }
            record.online = Boolean(summary.online);
            record.name = summary.name || record.name;

            if (!record.config || Date.now() - record.configAt > CONFIG_MS) {
                try {
                    const detail = await api(`/api/devices/${encodeURIComponent(summary.id)}`);
                    if (detail.config) {
                        record.config = detail.config;
                        record.configAt = Date.now();
                        await ensureEndpoints(record, summary, detail.config);
                        if (detail.lastData) summary.lastData = detail.lastData;
                    }
                } catch (error) {
                    log(`config fetch failed for ${summary.id}: ${error.message}`);
                }
            }
            if (!record.config) {
                log(`waiting for config for ${summary.id}`);
                continue;
            }
            await applyState(record, summary);
        }

        for (const [id, record] of records) {
            if (!seen.has(id)) await setReachable(record, false);
        }
    } catch (error) {
        log(`poll failed: ${error.message}`);
    } finally {
        polling = false;
    }
}

log(`server ${SERVER_URL}, device filter ${DEVICE_FILTER || "(all confirmed)"}`);
log(`poll every ${POLL_MS} ms, config refresh every ${CONFIG_MS} ms`);
log(`Matter port ${MATTER_PORT}, storage ${STORAGE_PATH}`);
log(`Matter vendor 0x${VENDOR_ID.toString(16)}, product 0x${PRODUCT_ID.toString(16)}`);

await poll();
const timer = setInterval(() => {
    void poll();
}, POLL_MS);

let stopping = false;
async function shutdown(signal) {
    if (stopping) return;
    stopping = true;
    log(`${signal}: shutting down`);
    clearInterval(timer);
    try {
        await server.close();
    } catch (error) {
        log(`shutdown error: ${error.message}`);
    }
    process.exit(0);
}

process.on("SIGINT", () => void shutdown("SIGINT"));
process.on("SIGTERM", () => void shutdown("SIGTERM"));
