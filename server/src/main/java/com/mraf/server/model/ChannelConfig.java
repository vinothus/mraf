package com.mraf.server.model;

import com.fasterxml.jackson.annotation.JsonProperty;

import java.util.ArrayList;
import java.util.List;

public class ChannelConfig {
    public int id;
    public String name;
    public String zone = "";
    public String equipment = "other";
    @JsonProperty("pulse_mode")
    public boolean pulseMode;
    @JsonProperty("pulse_on_ms")
    public int pulseOnMs = 600;
    @JsonProperty("pulse_off_ms")
    public int pulseOffMs = 600;
    public boolean enabled;
    public int days;
    public String mode = "auto";
    public List<ScheduleWindow> windows = new ArrayList<>();

    public ChannelConfig() {
    }

    public ChannelConfig(int id, String name, boolean enabled, int days, String mode,
                         List<ScheduleWindow> windows) {
        this.id = id;
        this.name = name;
        this.enabled = enabled;
        this.days = days;
        this.mode = mode;
        this.windows = windows;
    }

    public ChannelConfig(int id, String name, String zone, String equipment,
                         boolean enabled, int days, String mode,
                         List<ScheduleWindow> windows) {
        this(id, name, enabled, days, mode, windows);
        this.zone = zone;
        this.equipment = equipment;
    }
}
