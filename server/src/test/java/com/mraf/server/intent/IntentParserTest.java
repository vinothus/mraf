package com.mraf.server.intent;

import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

class IntentParserTest {
    private final IntentParser parser = new IntentParser();

    @Test
    void turnsRelayOn() {
        IntentParser.Intent i = parser.parse("turn on relay 3");
        assertEquals("control", i.action());
        assertEquals("relay", i.target());
        assertEquals(3, i.relayId());
        assertEquals("on", i.mode());
        assertTrue(i.understood());
    }

    @Test
    void relayIdBeforeMode() {
        IntentParser.Intent i = parser.parse("relay 5 off");
        assertEquals("relay", i.target());
        assertEquals(5, i.relayId());
        assertEquals("off", i.mode());
    }

    @Test
    void autoMode() {
        assertEquals("auto", parser.parse("set relay 1 to auto").mode());
    }

    @Test
    void pulseRelay() {
        IntentParser.Intent i = parser.parse("pulse relay 2 for 800 ms");
        assertEquals("relay", i.target());
        assertEquals(2, i.relayId());
        assertEquals(800, i.pulseMs());
        assertEquals("on", i.mode());
    }

    @Test
    void pumpOff() {
        IntentParser.Intent i = parser.parse("turn off the pump");
        assertEquals("pump", i.target());
        assertEquals("off", i.mode());
    }

    @Test
    void boostPump() {
        IntentParser.Intent i = parser.parse("boost the pump for 45 minutes");
        assertEquals("pump", i.target());
        assertEquals("auto", i.mode());
        assertEquals(45, i.boostMin());
    }

    @Test
    void boostDefaultsTo30() {
        assertEquals(30, parser.parse("boost pump").boostMin());
    }

    @Test
    void allOff() {
        IntentParser.Intent i = parser.parse("turn everything off");
        assertEquals("all", i.target());
        assertEquals("off", i.mode());
    }

    @Test
    void allAuto() {
        IntentParser.Intent i = parser.parse("all auto");
        assertEquals("all", i.target());
        assertEquals("auto", i.mode());
    }

    @Test
    void statusIntent() {
        assertEquals("status", parser.parse("what is the status").action());
        assertEquals("status", parser.parse("report").action());
    }

    @Test
    void dataIntent() {
        assertEquals("data", parser.parse("send data now").action());
    }

    @Test
    void helpIntent() {
        assertEquals("help", parser.parse("help").action());
        assertEquals("help", parser.parse("").action());
    }

    @Test
    void relayOutOfRange() {
        IntentParser.Intent i = parser.parse("turn on relay 20");
        assertFalse(i.understood());
        assertTrue(i.reply().contains("does not exist"));
    }

    @Test
    void unknownPhrase() {
        IntentParser.Intent i = parser.parse("make me a sandwich");
        assertFalse(i.understood());
        assertEquals("unknown", i.action());
        assertTrue(i.mode().isEmpty());
    }

    @Test
    void allOnDowngradesToAuto() {
        IntentParser.Intent i = parser.parse("turn all on");
        assertEquals("all", i.target());
        assertEquals("auto", i.mode());
    }
}
