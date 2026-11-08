package com.mraf.server.model;

import java.util.ArrayList;
import java.util.List;

public class ControllerConfig {
    public String profile = "agri";
    public List<ChannelConfig> channels = new ArrayList<>();
    public List<RecurringSchedule> recurring = new ArrayList<>();
    public PumpConfig pump = new PumpConfig();

    public static ControllerConfig demoDefaults() {
        ControllerConfig cfg = new ControllerConfig();
        cfg.channels.add(ch(1, "Shed Light", "Zone A", "light", true, 127, "18:00", "22:00"));
        cfg.channels.add(ch(2, "Yard Light", "Zone A", "light", true, 127, "18:30", "05:30"));
        cfg.channels.add(ch(3, "Drip Zone A", "Zone B", "valve", true, 21, "06:00", "06:30"));
        cfg.channels.add(ch(4, "Drip Zone B", "Zone C", "valve", true, 21, "06:30", "07:00"));
        cfg.channels.add(ch(5, "Aerator 1", "Pond", "motor", true, 127, "22:00", "04:00"));
        cfg.channels.add(ch(6, "Aerator 2", "Pond", "motor", true, 127, "22:00", "04:00"));
        cfg.channels.add(ch(7, "Cattle Fan", "Shed", "fan", true, 127, "10:00", "16:00"));
        cfg.channels.add(ch(8, "Milking Light", "Shed", "light", true, 127, "04:30", "06:00"));
        cfg.channels.add(ch(9, "Gate Light", "Gate", "light", true, 127, "19:00", "06:00"));
        cfg.channels.add(new ChannelConfig(10, "Spare 1", "Zone 10", "other",
                false, 0, "auto", new ArrayList<>()));
        cfg.channels.add(new ChannelConfig(11, "Spare 2", "Zone 11", "other",
                false, 0, "auto", new ArrayList<>()));
        cfg.channels.add(new ChannelConfig(12, "Spare 3", "Zone 12", "other",
                false, 0, "auto", new ArrayList<>()));
        cfg.channels.get(4).pulseMode = true;
        cfg.channels.get(5).pulseMode = true;
        cfg.pump.windows = new ArrayList<>(List.of(
                new ScheduleWindow("06:00", "07:00"),
                new ScheduleWindow("17:00", "18:00")));
        return cfg;
    }

    private static ChannelConfig ch(int id, String name, String zone, String equipment,
                                    boolean enabled, int days, String on, String off) {
        List<ScheduleWindow> windows = new ArrayList<>();
        windows.add(new ScheduleWindow(on, off));
        return new ChannelConfig(id, name, zone, equipment, enabled, days, "auto", windows);
    }
}
