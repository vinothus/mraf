package com.mraf.server.model;

import com.fasterxml.jackson.annotation.JsonProperty;

import java.util.ArrayList;
import java.util.List;

public class RecurringSchedule {
    public String name = "Schedule";
    public boolean enabled = true;
    public String repeat = "daily";
    public int days = 127;
    @JsonProperty("day_of_month")
    public int dayOfMonth = 1;
    public int month = 1;
    public String from = "06:00";
    public String to = "07:00";
    @JsonProperty("channel_mask")
    public int channelMask;
    public List<Integer> channels = new ArrayList<>();
    public String action = "on";

    public RecurringSchedule() {
    }
}
