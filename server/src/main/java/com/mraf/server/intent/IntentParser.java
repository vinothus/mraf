package com.mraf.server.intent;

import org.springframework.stereotype.Component;

import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Deterministic, offline intent parser shared by the REST chat endpoint, the
 * MCP {@code chat} tool and (mirrored) the ESP32 firmware. No language model is
 * involved: a small keyword grammar maps an utterance onto a controller action.
 */
@Component
public class IntentParser {
    public record Intent(String action, String target, int relayId, String mode,
                         Integer pulseMs, Integer boostMin, String reply, boolean understood) {

        static Intent help(String reply) {
            return new Intent("help", "", 0, "", null, null, reply, true);
        }

        static Intent unknown(String reply) {
            return new Intent("unknown", "", 0, "", null, null, reply, false);
        }
    }

    private static final Pattern RELAY_ID = Pattern.compile(
            "\\b(?:relay|channel|output|zone|light|valve|fan|motor|socket|heater|door)\\s*#?\\s*(\\d{1,2})\\b");
    private static final Pattern HASH_ID = Pattern.compile("#\\s*(\\d{1,2})\\b");
    private static final Pattern PULSE_MS = Pattern.compile("(\\d{2,5})\\s*(?:ms|millis|milliseconds)");
    private static final Pattern BOOST_MIN = Pattern.compile("(\\d{1,4})\\s*(?:min|minute|minutes)");
    private static final Pattern WORD_ON = Pattern.compile("\\b(on|start|run|enable|enabled|activate)\\b");
    private static final Pattern WORD_OFF = Pattern.compile("\\b(off|stop|disable|disabled|deactivate|kill)\\b");
    private static final Pattern WORD_AUTO = Pattern.compile("\\b(auto|automatic|automatic mode|scheduled)\\b");

    public Intent parse(String text) {
        String raw = text == null ? "" : text.trim();
        if (raw.isBlank()) {
            return Intent.help("Say things like 'turn on relay 3', 'pump off', 'all auto', "
                    + "'boost pump 30 min' or 'status'.");
        }
        String t = raw.toLowerCase(Locale.ROOT)
                .replaceAll("[^a-z0-9#:.\\s]", " ")
                .replaceAll("\\s+", " ")
                .trim();

        if (t.matches("^(help|commands|what can you do.*|how do i.*|\\?)$")) {
            return Intent.help(helpText());
        }

        boolean wantsStatus = contains(t, "status", "report", "summary")
                || (t.contains("how") && (t.contains(" is ") || t.contains(" are ") || t.startsWith("how is") || t.startsWith("how are")));
        boolean wantsData = t.equals("data")
                || (contains(t, "data", "telemetry", "reading", "readings", "sensors", "sensor")
                    && contains(t, "request", "get", "fetch", "read", "send", "give", "now", "poll"));

        if (wantsStatus) {
            return new Intent("status", "", 0, "", null, null,
                    "Fetching the latest controller status.", true);
        }
        if (wantsData) {
            return new Intent("data", "", 0, "", null, null,
                    "Requesting a fresh status snapshot from the controller.", true);
        }

        boolean all = contains(t, "all", "everything", "every relay", "every output");
        boolean pump = contains(t, "pump", "borewell", "contactor");
        Integer pulseMs = matchInt(PULSE_MS, t);
        Integer boostMin = matchInt(BOOST_MIN, t);

        if (contains(t, "boost")) {
            int minutes = boostMin == null ? 30 : boostMin;
            return new Intent("control", "pump", 0, "auto", null, minutes,
                    "Boosting the pump for " + minutes + " minutes.", true);
        }

        String mode = detectMode(t);

        if (all) {
            if (mode == null) {
                return Intent.unknown("Tell me whether to turn everything on, off or to auto.");
            }
            if ("on".equals(mode)) {
                return new Intent("control", "all", 0, "auto", null, null,
                        "Turning all outputs on is not allowed; using auto instead.", true);
            }
            return new Intent("control", "all", 0, mode, null, null,
                    "Setting all outputs to " + mode + ".", true);
        }

        Integer relayId = matchRelay(t);

        if (pump && relayId == null) {
            if (mode == null) {
                return Intent.unknown("Tell me whether to turn the pump on, off or to auto.");
            }
            return new Intent("control", "pump", 0, mode, null, null,
                    "Setting the pump to " + mode + ".", true);
        }

        if (relayId != null) {
            if (relayId < 1 || relayId > 12) {
                return Intent.unknown("Relay " + relayId + " does not exist; valid ids are 1..12.");
            }
            if (pulseMs != null) {
                return new Intent("control", "relay", relayId, "on", pulseMs, null,
                        "Pulsing relay " + relayId + " for " + pulseMs + " ms.", true);
            }
            if (mode == null) {
                return Intent.unknown("Tell me whether to turn relay " + relayId + " on, off or to auto.");
            }
            return new Intent("control", "relay", relayId, mode, null, null,
                    "Setting relay " + relayId + " to " + mode + ".", true);
        }

        if (mode != null) {
            return Intent.unknown("Which output should I switch? Mention a relay number or the pump.");
        }
        return Intent.unknown("I did not understand that. " + helpText());
    }

    private static String helpText() {
        return "Try: 'turn on relay 3', 'relay 5 off', 'pulse relay 2 for 800 ms', "
                + "'pump on', 'boost pump 30 min', 'all off', 'all auto', 'status'.";
    }

    private static String detectMode(String t) {
        if (WORD_AUTO.matcher(t).find()) {
            return "auto";
        }
        if (WORD_OFF.matcher(t).find()) {
            return "off";
        }
        if (WORD_ON.matcher(t).find()) {
            return "on";
        }
        return null;
    }

    private static Integer matchRelay(String t) {
        Matcher named = RELAY_ID.matcher(t);
        if (named.find()) {
            return Integer.parseInt(named.group(1));
        }
        Matcher hash = HASH_ID.matcher(t);
        if (hash.find()) {
            return Integer.parseInt(hash.group(1));
        }
        return null;
    }

    private static Integer matchInt(Pattern pattern, String t) {
        Matcher m = pattern.matcher(t);
        if (m.find()) {
            return Integer.parseInt(m.group(1));
        }
        return null;
    }

    private static boolean contains(String haystack, String... needles) {
        for (String needle : needles) {
            if (haystack.contains(needle)) {
                return true;
            }
        }
        return false;
    }
}
