package com.mraf.server.model;

public record RelayDefinition(int id, int gpio, String moduleInput, String note) {
    public static RelayDefinition[] esp32Defaults() {
        int[] pins = {13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27};
        RelayDefinition[] rows = new RelayDefinition[pins.length];
        for (int i = 0; i < pins.length; i++) {
            rows[i] = new RelayDefinition(i + 1, pins[i], "IN" + (i + 1),
                    i == 0 ? "Scheduled load" : "");
        }
        return rows;
    }
}
