package com.mraf.server.model;

import com.fasterxml.jackson.annotation.JsonProperty;

import java.util.ArrayList;
import java.util.List;

public class PumpConfig {
    public String name = "5HP Borewell Pump";
    public boolean enabled = true;
    public int days = 127;
    public String mode = "auto";
    public List<ScheduleWindow> windows = new ArrayList<>();
    @JsonProperty("min_off_sec")
    public int minOffSec = 300;
    @JsonProperty("max_run_min")
    public int maxRunMin = 120;
    @JsonProperty("recovery_delay_sec")
    public int recoveryDelaySec = 30;
    @JsonProperty("resume_after_outage")
    public boolean resumeAfterOutage = true;
    @JsonProperty("dry_run_current_a")
    public double dryRunCurrentA = 1.5;
    @JsonProperty("dry_run_grace_sec")
    public int dryRunGraceSec = 20;
}
