import test from "node:test";
import assert from "node:assert/strict";

import {
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
} from "../src/mapping.mjs";

test("endpoint ids are stable, sanitized and bounded", () => {
    const a = endpointId("ESP32-A1B2C3", "r1");
    const b = endpointId("ESP32-A1B2C3", "r1");
    assert.equal(a, b);
    assert.match(a, /^mraf-ESP32-A1B2C3-[0-9a-f]{6}-r1$/);
    assert.ok(a.length <= 64);

    const weird = endpointId("farm/device with spaces", "pump");
    assert.match(weird, /^mraf-farm_device_with_spaces-[0-9a-f]{6}-pump$/);
    assert.ok(weird.length <= 64);
});

test("labels are short and include zone when present", () => {
    assert.equal(truncate("abcdef", 4), "abcd");
    assert.equal(labelFor("Farm", { id: 1, name: "Shed Light", zone: "Shed" }), "Farm - Shed Shed Light");
    assert.equal(labelFor("Farm", { id: 1, name: "Light" }), "Farm - Light");
    assert.ok(labelFor("x".repeat(40), { id: 1, name: "Light" }).length <= 32);
    assert.equal(pumpLabel("Farm"), "Farm - Pump");
    assert.equal(sensorLabel("Farm", "tank"), "Farm - Tank full");
    assert.equal(sensorLabel("Farm", "sump"), "Farm - Sump dry");
});

test("equipment maps to light or plug-in unit", () => {
    assert.equal(relayDeviceKind("light"), "light");
    assert.equal(relayDeviceKind("motor"), "plug");
    assert.equal(relayDeviceKind(undefined), "plug");
});

test("relay commands use pulse timing only for pulse channels", () => {
    assert.deepEqual(relayControl({ id: 3, pulse_mode: false }, true), {
        target: "relay",
        id: 3,
        mode: "on",
    });
    assert.deepEqual(
        relayControl({ id: 5, pulse_mode: true, pulse_on_ms: 800, pulse_off_ms: 400 }, true),
        { target: "relay", id: 5, mode: "on", pulse_ms: 800 },
    );
    assert.deepEqual(
        relayControl({ id: 5, pulse_mode: true, pulse_on_ms: 800, pulse_off_ms: 400 }, false),
        { target: "relay", id: 5, mode: "off", pulse_ms: 400 },
    );
    assert.deepEqual(relayControl({ id: 5, pulse_mode: true }, true), {
        target: "relay",
        id: 5,
        mode: "on",
        pulse_ms: 600,
    });
});

test("pump control maps on/off", () => {
    assert.deepEqual(pumpControl(true), { target: "pump", mode: "on" });
    assert.deepEqual(pumpControl(false), { target: "pump", mode: "off" });
});

test("channelsOf supplies twelve fallback channels", () => {
    const channels = channelsOf(null);
    assert.equal(channels.length, 12);
    assert.equal(channels[0].id, 1);
    assert.equal(channels[11].id, 12);
    assert.equal(channelsOf({ channels: [{ id: 7 }] }).length, 1);
});

test("snapshotState normalizes missing data", () => {
    const state = snapshotState(null);
    assert.equal(state.relays.length, 12);
    assert.ok(state.relays.every(value => value === false));
    assert.equal(state.pumpOn, false);
    assert.equal(state.tankFull, false);
    assert.equal(state.sumpDry, false);

    const live = snapshotState({
        relays: [1, 0, 1],
        pump: { on: true },
        tank_full: true,
        sump_dry: false,
    });
    assert.deepEqual(live.relays.slice(0, 4), [true, false, true, false]);
    assert.equal(live.pumpOn, true);
    assert.equal(live.tankFull, true);
});
