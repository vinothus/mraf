package com.mraf.server.service;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.mraf.server.model.ChannelConfig;
import com.mraf.server.model.ControllerConfig;
import com.mraf.server.model.PumpConfig;
import com.mraf.server.model.RecurringSchedule;
import com.mraf.server.model.RelayDefinition;
import com.mraf.server.db.DeviceEntity;
import com.mraf.server.db.DeviceRepository;
import com.mraf.server.db.DeviceRequestEntity;
import com.mraf.server.db.DeviceRequestRepository;
import com.mraf.server.model.ScheduleWindow;
import jakarta.annotation.PostConstruct;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.security.crypto.factory.PasswordEncoderFactories;
import org.springframework.security.crypto.password.PasswordEncoder;
import org.springframework.stereotype.Service;

import java.time.Instant;
import java.time.ZoneId;
import java.time.format.DateTimeFormatter;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.Deque;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.regex.Pattern;

@Service
public class RemoteStateService {
    private static final Logger log = LoggerFactory.getLogger(RemoteStateService.class);
    private static final Pattern TIME = Pattern.compile("^([01]\\d|2[0-3]):[0-5]\\d$");
    private static final int MAX_EVENTS = 40;
    private static final int MAX_ACKS = 20;
    private static final Set<String> PROFILES =
            Set.of("agri", "home", "godown", "industry");
    private static final Set<String> EQUIPMENT =
            Set.of("valve", "light", "motor", "fan", "pump", "socket", "heater",
                    "door", "other");
    private static final Set<String> REPEATS =
            Set.of("daily", "weekly", "monthly", "yearly");
    private static final Set<String> ACTIONS = Set.of("on", "off", "auto");
    private static final DateTimeFormatter ISO =
            DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss")
                    .withZone(ZoneId.systemDefault());

    private final ObjectMapper mapper;
    private final String configuredToken;
    private final String serverTimezone;
    private final DeviceRepository deviceRepo;
    private final DeviceRequestRepository requestRepo;
    private final PasswordEncoder encoder =
            PasswordEncoderFactories.createDelegatingPasswordEncoder();
    private final Map<String, Device> devices = new LinkedHashMap<>();

    public RemoteStateService(ObjectMapper mapper,
                              DeviceRepository deviceRepo,
                              DeviceRequestRepository requestRepo,
                              @Value("${mraf.token:}") String configuredToken,
                              @Value("${mraf.server-timezone:Asia/Kolkata}") String serverTimezone) {
        this.mapper = mapper;
        this.deviceRepo = deviceRepo;
        this.requestRepo = requestRepo;
        this.configuredToken = configuredToken == null ? "" : configuredToken;
        this.serverTimezone = serverTimezone;
    }

    // ---------------------------------------------------------------------
    // Persistence
    // ---------------------------------------------------------------------
    @PostConstruct
    void loadRegistry() {
        try {
            for (DeviceEntity e : deviceRepo.findAll()) {
                Device d = new Device(e.id);
                d.name = e.name == null ? "" : e.name;
                d.firmware = e.firmware == null ? "" : e.firmware;
                d.profile = validProfile(e.profile);
                d.confirmed = e.confirmed;
                d.rejected = e.rejected;
                d.remoteUser = e.remoteUser == null ? "" : e.remoteUser;
                d.passwordHash = e.passwordHash == null ? "" : e.passwordHash;
                d.pollIntervalSec = clampInterval(e.pollIntervalSec);
                d.firstSeen = e.firstSeen == null ? Instant.now() : e.firstSeen;
                d.lastPollAt = e.lastPollAt;
                d.pollCount = e.pollCount;
                d.requestSeq = e.requestSeq;
                d.config = readConfig(e.configJson, d.id);
                d.lastData = readJson(e.lastDataJson);
                devices.put(d.id, d);
            }
            for (DeviceRequestEntity r : requestRepo.findByStatusNotOrderByCreatedAtAsc("ACKED")) {
                Device d = devices.get(r.deviceId);
                if (d == null) {
                    continue;
                }
                RemoteRequest req = new RemoteRequest(r.id, RequestType.valueOf(r.type),
                        readObject(r.payloadJson));
                req.attempts = r.attempts;
                req.createdAt = r.createdAt == null ? Instant.now() : r.createdAt;
                if ("INFLIGHT".equals(r.status)) {
                    d.inflight = req;
                } else {
                    d.queue.addLast(req);
                }
            }
            log.info("Loaded {} device(s) and {} pending request(s) from H2",
                    devices.size(), requestRepo.count());
        } catch (Exception e) {
            log.warn("Could not load devices from the database", e);
        }
    }

    private ControllerConfig readConfig(String json, String id) {
        if (json == null || json.isBlank()) {
            return ControllerConfig.demoDefaults();
        }
        try {
            return validateConfig(mapper.readValue(json, ControllerConfig.class));
        } catch (Exception e) {
            log.warn("Invalid saved config for device {}; using defaults", id, e);
            return ControllerConfig.demoDefaults();
        }
    }

    private JsonNode readJson(String json) {
        if (json == null || json.isBlank()) {
            return null;
        }
        try {
            return mapper.readTree(json);
        } catch (Exception e) {
            return null;
        }
    }

    private ObjectNode readObject(String json) {
        JsonNode node = readJson(json);
        return node != null && node.isObject() ? (ObjectNode) node : mapper.createObjectNode();
    }

    private void persist(Device dev) {
        try {
            DeviceEntity e = deviceRepo.findById(dev.id).orElseGet(DeviceEntity::new);
            e.id = dev.id;
            e.name = dev.name;
            e.firmware = dev.firmware;
            e.profile = dev.profile;
            e.confirmed = dev.confirmed;
            e.rejected = dev.rejected;
            e.remoteUser = dev.remoteUser;
            e.passwordHash = dev.passwordHash;
            e.pollIntervalSec = dev.pollIntervalSec;
            e.firstSeen = dev.firstSeen;
            e.lastPollAt = dev.lastPollAt;
            e.pollCount = dev.pollCount;
            e.requestSeq = dev.requestSeq;
            e.configJson = mapper.writeValueAsString(dev.config);
            e.lastDataJson = dev.lastData == null ? null : dev.lastData.toString();
            deviceRepo.save(e);
        } catch (Exception ex) {
            log.warn("Could not save device {}", dev.id, ex);
            addEvent(dev, "warn", "Could not save device: " + ex.getMessage());
        }
    }

    private void persistRequest(Device dev, RemoteRequest r, String status) {
        try {
            DeviceRequestEntity e = requestRepo.findById(r.id).orElseGet(DeviceRequestEntity::new);
            e.id = r.id;
            e.deviceId = dev.id;
            e.type = r.type.name();
            e.status = status;
            e.payloadJson = r.payload == null ? "{}" : r.payload.toString();
            e.attempts = r.attempts;
            e.createdAt = r.createdAt;
            if (!"QUEUED".equals(status) && e.deliveredAt == null) {
                e.deliveredAt = Instant.now();
            }
            if ("ACKED".equals(status)) {
                e.ackedAt = Instant.now();
                e.result = r.result;
            }
            requestRepo.save(e);
        } catch (Exception ex) {
            log.warn("Could not save request {} for device {}", r.id, dev.id, ex);
        }
    }

    private void markAcked(String requestId, String result) {
        try {
            requestRepo.findById(requestId).ifPresent(e -> {
                e.status = "ACKED";
                e.ackedAt = Instant.now();
                e.result = result;
                requestRepo.save(e);
            });
        } catch (Exception ex) {
            log.warn("Could not mark request {} acked", requestId, ex);
        }
    }

    // ---------------------------------------------------------------------
    // Poll handling / registration
    // ---------------------------------------------------------------------
    public boolean tokenMatches(String token) {
        return configuredToken.isBlank() || configuredToken.equals(token == null ? "" : token);
    }

    public synchronized JsonNode handlePoll(JsonNode request) {
        String id = request.path("device_id").asText("").trim();
        if (id.isBlank()) {
            id = request.path("device").asText("").trim();
        }
        if (id.isBlank()) {
            throw new IllegalArgumentException("device_id is required");
        }

        Device dev = devices.get(id);
        boolean created = false;
        if (dev == null) {
            dev = new Device(id);
            devices.put(id, dev);
            created = true;
        }
        if (request.hasNonNull("device")) {
            dev.name = request.path("device").asText("");
        }
        if (request.hasNonNull("fw")) {
            dev.firmware = request.path("fw").asText("");
        }
        String reportedProfile = request.path("profile").asText("");
        if (created && PROFILES.contains(reportedProfile)) {
            dev.profile = reportedProfile;
        }
        dev.lastPollAt = Instant.now();
        dev.pollCount++;

        String authUser = request.path("auth").path("user").asText("");
        String authPass = request.path("auth").path("pass").asText("");
        boolean register = request.path("register").asBoolean(false);

        if (dev.rejected) {
            persist(dev);
            return meta(registerResponse("rejected", "registration rejected by admin"), dev);
        }

        if (!dev.confirmed) {
            if (register) {
                String err = registerCredentials(dev, authUser, authPass);
                if (err != null) {
                    persist(dev);
                    return meta(registerResponse("credentials_required", err), dev);
                }
            }
            if (created) {
                addEvent(dev, "register", "New device registered: " + id);
            }
            persist(dev);
            return meta(registerResponse("pending", "waiting for admin confirmation"), dev);
        }

        // Confirmed devices must present the credentials they registered with.
        if (dev.remoteUser.isBlank()) {
            if (!authUser.isBlank()) {
                String err = registerCredentials(dev, authUser, authPass);
                if (err != null) {
                    persist(dev);
                    return meta(authResponse(err), dev);
                }
            }
        } else if (!dev.remoteUser.equals(authUser)
                || !encoder.matches(authPass == null ? "" : authPass, dev.passwordHash)) {
            persist(dev);
            return meta(authResponse("device credentials rejected"), dev);
        }

        if (request.has("data") && request.get("data").isObject()) {
            dev.lastData = request.get("data").deepCopy();
            addEvent(dev, "data", "Controller sent a status snapshot");
        }

        JsonNode ack = request.get("ack");
        if (ack != null && !ack.asText("").isBlank()) {
            recordAck(dev, ack.asText(), request.path("result").asText("done"));
        }

        if (dev.lastData == null && dev.inflight == null && dev.queue.isEmpty()) {
            queueDataInternal(dev);
            addEvent(dev, "queue", "Auto-queued initial data request");
        }

        RemoteRequest outgoing = dev.inflight;
        if (outgoing == null) {
            outgoing = dev.queue.pollFirst();
            if (outgoing != null) {
                dev.inflight = outgoing;
                outgoing.attempts++;
                persistRequest(dev, outgoing, "INFLIGHT");
                addEvent(dev, "poll", "Sent " + outgoing.type.name().toLowerCase(Locale.ROOT)
                        + " request " + outgoing.id);
            }
        } else {
            outgoing.attempts++;
            persistRequest(dev, outgoing, "INFLIGHT");
            addEvent(dev, "poll", "Re-sent " + outgoing.type.name().toLowerCase(Locale.ROOT)
                    + " request " + outgoing.id + " while waiting for ack");
        }
        persist(dev);

        ObjectNode response;
        if (outgoing == null) {
            response = mapper.createObjectNode();
            response.put("request", "none");
        } else {
            response = outgoing.toResponse(mapper);
        }
        return meta(response, dev);
    }

    private String registerCredentials(Device dev, String user, String pass) {
        if (user == null || user.isBlank()) {
            return "server login user is required";
        }
        if (pass == null || pass.isBlank()) {
            return "server login password is required";
        }
        for (Device other : devices.values()) {
            if (other != dev && user.equals(other.remoteUser)) {
                return "server login user is already used by another device";
            }
        }
        if (dev.passwordHash.isBlank()) {
            dev.remoteUser = user;
            dev.passwordHash = encoder.encode(pass);
            addEvent(dev, "register", "Stored server login for user " + user);
            return null;
        }
        if (!dev.remoteUser.equals(user) || !encoder.matches(pass, dev.passwordHash)) {
            return "server login credentials do not match the registered device";
        }
        return null;
    }

    private ObjectNode registerResponse(String status, String message) {
        ObjectNode out = mapper.createObjectNode();
        out.put("request", "register");
        out.put("status", status);
        out.put("message", message);
        return out;
    }

    private ObjectNode authResponse(String message) {
        ObjectNode out = mapper.createObjectNode();
        out.put("request", "auth");
        out.put("status", "denied");
        out.put("message", message);
        return out;
    }

    private ObjectNode meta(ObjectNode response, Device dev) {
        response.put("confirmed", dev.confirmed);
        response.put("profile", dev.profile);
        response.put("poll_interval_sec", dev.pollIntervalSec);
        return response;
    }

    private void recordAck(Device dev, String id, String result) {
        ObjectNode ack = mapper.createObjectNode();
        ack.put("id", id);
        ack.put("result", result);
        ack.put("at", Instant.now().toString());
        dev.lastAck = ack;
        dev.ackHistory.addFirst(ack);
        while (dev.ackHistory.size() > MAX_ACKS) {
            dev.ackHistory.removeLast();
        }

        markAcked(id, result);
        if (dev.inflight != null && dev.inflight.id.equals(id)) {
            RemoteRequest done = dev.inflight;
            dev.inflight = null;
            addEvent(dev, "ack", done.type.name().toLowerCase(Locale.ROOT)
                    + " " + id + " -> " + result);
        } else {
            addEvent(dev, "ack", "Late ack " + id + " -> " + result);
        }
    }

    // ---------------------------------------------------------------------
    // Admin operations
    // ---------------------------------------------------------------------
    public synchronized ObjectNode confirmDevice(String id, JsonNode body) {
        Device dev = requireDevice(id);
        dev.confirmed = true;
        dev.rejected = false;
        if (body != null) {
            String profile = body.path("profile").asText("");
            if (PROFILES.contains(profile)) {
                dev.profile = profile;
            }
            if (body.has("poll_interval_sec")) {
                dev.pollIntervalSec = clampInterval(body.path("poll_interval_sec").asInt(60));
            }
        }
        persist(dev);
        addEvent(dev, "admin", "Device confirmed by admin");
        return summary(dev);
    }

    public synchronized ObjectNode rejectDevice(String id) {
        Device dev = requireDevice(id);
        dev.confirmed = false;
        dev.rejected = true;
        persist(dev);
        addEvent(dev, "admin", "Device registration rejected by admin");
        return summary(dev);
    }

    public synchronized ObjectNode updateDevice(String id, JsonNode body) {
        Device dev = requireDevice(id);
        if (body != null) {
            String profile = body.path("profile").asText("");
            if (PROFILES.contains(profile)) {
                dev.profile = profile;
            }
            if (body.has("poll_interval_sec")) {
                dev.pollIntervalSec = clampInterval(body.path("poll_interval_sec").asInt(60));
            }
        }
        persist(dev);
        addEvent(dev, "admin", "Device settings updated by admin");
        return summary(dev);
    }

    public synchronized void deleteDevice(String id) {
        Device dev = devices.remove(id);
        if (dev != null) {
            try {
                requestRepo.deleteByDeviceId(id);
                deviceRepo.deleteById(id);
            } catch (Exception e) {
                log.warn("Could not delete device {}", id, e);
            }
        }
    }

    public synchronized List<String> deviceIds() {
        return new ArrayList<>(devices.keySet());
    }

    synchronized void clearAllDevices() {
        requestRepo.deleteAll();
        deviceRepo.deleteAll();
        devices.clear();
    }

    synchronized void reloadFromDatabase() {
        devices.clear();
        loadRegistry();
    }

    public synchronized ObjectNode queueDataRequest(String id) {
        Device dev = requireDevice(id);
        if (dev.inflight != null && dev.inflight.type == RequestType.DATA) {
            return requestSummary(dev, "already in flight", dev.inflight);
        }
        for (RemoteRequest r : dev.queue) {
            if (r.type == RequestType.DATA) {
                return requestSummary(dev, "already queued", r);
            }
        }
        RemoteRequest r = queueDataInternal(dev);
        persist(dev);
        addEvent(dev, "queue", "Queued data request " + r.id);
        return requestSummary(dev, "queued", r);
    }

    private RemoteRequest queueDataInternal(Device dev) {
        RemoteRequest r = new RemoteRequest(dev.id + "-data-" + (++dev.requestSeq),
                RequestType.DATA, mapper.createObjectNode());
        dev.queue.addLast(r);
        persistRequest(dev, r, "QUEUED");
        return r;
    }

    public synchronized ObjectNode queueControl(String id, JsonNode body) {
        Device dev = requireDevice(id);
        ArrayNode commands = mapper.createArrayNode();
        if (body.isArray()) {
            body.forEach(commands::add);
        } else if (body.has("commands") && body.get("commands").isArray()) {
            body.get("commands").forEach(commands::add);
        } else {
            commands.add(body);
        }
        if (commands.isEmpty()) {
            throw new IllegalArgumentException("at least one command is required");
        }
        commands.forEach(this::validateCommand);

        String rid = dev.id + "-ctl-" + (++dev.requestSeq);
        ObjectNode payload = mapper.createObjectNode();
        payload.set("commands", commands);
        RemoteRequest r = new RemoteRequest(rid, RequestType.CONTROL, payload);
        dev.queue.addLast(r);
        persistRequest(dev, r, "QUEUED");
        updateModesOptimistically(dev, commands);
        persist(dev);
        addEvent(dev, "queue", "Queued control request " + rid);
        return requestSummary(dev, "queued", r);
    }

    private void validateCommand(JsonNode command) {
        if (!command.isObject()) {
            throw new IllegalArgumentException("each command must be an object");
        }
        String target = command.path("target").asText("");
        if (!Set.of("relay", "pump", "all").contains(target)) {
            throw new IllegalArgumentException("target must be relay, pump or all");
        }
        String mode = command.path("mode").asText("auto");
        if (!Set.of("auto", "on", "off").contains(mode)) {
            throw new IllegalArgumentException("mode must be auto, on or off");
        }
        if ("all".equals(target) && "on".equals(mode)) {
            throw new IllegalArgumentException("all supports auto and off only");
        }
        if ("relay".equals(target)) {
            int id = command.path("id").asInt(0);
            if (id < 1 || id > 12) {
                throw new IllegalArgumentException("relay id must be 1..12");
            }
        }
        if (command.has("boost_min")) {
            int boost = command.path("boost_min").asInt();
            if (boost < 0 || boost > 1440) {
                throw new IllegalArgumentException("boost_min must be 0..1440");
            }
        }
        if (command.has("pulse_ms")) {
            int pulse = command.path("pulse_ms").asInt();
            if (pulse < 20 || pulse > 10000) {
                throw new IllegalArgumentException("pulse_ms must be 20..10000");
            }
        }
    }

    private void updateModesOptimistically(Device dev, ArrayNode commands) {
        for (JsonNode cmd : commands) {
            String target = cmd.path("target").asText();
            String mode = cmd.path("mode").asText("auto");
            if ("relay".equals(target)) {
                int id = cmd.path("id").asInt();
                dev.config.channels.stream().filter(c -> c.id == id)
                        .findFirst().ifPresent(c -> c.mode = mode);
            } else if ("pump".equals(target)) {
                dev.config.pump.mode = mode;
            } else if ("all".equals(target)) {
                dev.config.channels.forEach(c -> c.mode = mode);
                dev.config.pump.mode = mode;
            }
        }
    }

    public synchronized ObjectNode applyConfig(String id, ControllerConfig incoming) {
        Device dev = requireDevice(id);
        ControllerConfig normalized = validateConfig(incoming);
        dev.config = normalized;
        dev.profile = normalized.profile;

        ObjectNode payload = mapper.createObjectNode();
        payload.put("profile", normalized.profile);
        payload.set("channels", mapper.valueToTree(normalized.channels));
        payload.set("recurring", mapper.valueToTree(normalized.recurring));
        payload.set("pump", mapper.valueToTree(normalized.pump));

        String rid = dev.id + "-cfg-" + (++dev.requestSeq);
        RemoteRequest r = new RemoteRequest(rid, RequestType.CONFIG, payload);
        dev.queue.addLast(r);
        persistRequest(dev, r, "QUEUED");
        persist(dev);
        addEvent(dev, "queue", "Queued configuration request " + rid);
        return requestSummary(dev, "queued", r);
    }

    public synchronized void clearQueue(String id) {
        Device dev = requireDevice(id);
        int count = dev.queue.size() + (dev.inflight == null ? 0 : 1);
        dev.queue.clear();
        dev.inflight = null;
        try {
            requestRepo.deleteByDeviceIdAndStatusIn(id, List.of("QUEUED", "INFLIGHT"));
        } catch (Exception e) {
            log.warn("Could not clear queued requests for {}", id, e);
        }
        persist(dev);
        addEvent(dev, "queue", "Cleared " + count + " pending request(s)");
    }

    // ---------------------------------------------------------------------
    // Validation
    // ---------------------------------------------------------------------
    private ControllerConfig validateConfig(ControllerConfig incoming) {
        if (incoming == null || incoming.channels == null || incoming.channels.size() != 12) {
            throw new IllegalArgumentException("exactly 12 channels are required");
        }
        if (!PROFILES.contains(incoming.profile)) {
            throw new IllegalArgumentException("profile must be agri/home/godown/industry");
        }
        List<ChannelConfig> channels = new ArrayList<>(incoming.channels);
        channels.sort(Comparator.comparingInt(c -> c.id));
        Set<Integer> ids = new HashSet<>();
        for (int i = 0; i < channels.size(); i++) {
            ChannelConfig c = channels.get(i);
            if (c.id != i + 1 || !ids.add(c.id)) {
                throw new IllegalArgumentException("channel ids must be 1..12 in order");
            }
            checkName(c.name, "channel " + c.id);
            if (c.zone != null && c.zone.length() > 23) {
                throw new IllegalArgumentException("channel " + c.id + ": zone is too long");
            }
            if (!EQUIPMENT.contains(c.equipment)) {
                throw new IllegalArgumentException("channel " + c.id + ": unknown equipment");
            }
            if (c.pulseOnMs < 20 || c.pulseOnMs > 10000
                    || c.pulseOffMs < 20 || c.pulseOffMs > 10000) {
                throw new IllegalArgumentException(
                        "channel " + c.id + ": pulse must be 20..10000 ms");
            }
            if (c.days < 0 || c.days > 127) {
                throw new IllegalArgumentException("channel " + c.id + ": days must be 0..127");
            }
            checkMode(c.mode, "channel " + c.id);
            checkWindows(c.windows, "channel " + c.id);
        }
        PumpConfig p = incoming.pump;
        if (p == null) {
            throw new IllegalArgumentException("pump config is required");
        }
        checkName(p.name, "pump");
        if (p.days < 0 || p.days > 127) {
            throw new IllegalArgumentException("pump: days must be 0..127");
        }
        checkMode(p.mode, "pump");
        checkWindows(p.windows, "pump");
        if (p.minOffSec < 30 || p.minOffSec > 3600) {
            throw new IllegalArgumentException("pump: min_off_sec must be 30..3600");
        }
        if (p.maxRunMin < 1 || p.maxRunMin > 720) {
            throw new IllegalArgumentException("pump: max_run_min must be 1..720");
        }
        if (p.recoveryDelaySec < 0 || p.recoveryDelaySec > 600) {
            throw new IllegalArgumentException("pump: recovery_delay_sec must be 0..600");
        }
        if (p.dryRunCurrentA < 0.5 || p.dryRunCurrentA > 20.0) {
            throw new IllegalArgumentException("pump: dry_run_current_a must be 0.5..20");
        }
        if (p.dryRunGraceSec < 5 || p.dryRunGraceSec > 120) {
            throw new IllegalArgumentException("pump: dry_run_grace_sec must be 5..120");
        }
        List<RecurringSchedule> recurring = incoming.recurring == null
                ? new ArrayList<>() : new ArrayList<>(incoming.recurring);
        if (recurring.size() > 8) {
            throw new IllegalArgumentException("recurring: at most 8 schedules are allowed");
        }
        for (int i = 0; i < recurring.size(); i++) {
            RecurringSchedule rs = recurring.get(i);
            String label = "recurring " + (i + 1);
            if (rs == null) {
                throw new IllegalArgumentException(label + ": must be an object");
            }
            checkName(rs.name, label);
            if (!REPEATS.contains(rs.repeat)) {
                throw new IllegalArgumentException(label + ": repeat must be daily/weekly/monthly/yearly");
            }
            if (!ACTIONS.contains(rs.action)) {
                throw new IllegalArgumentException(label + ": action must be on/off/auto");
            }
            if (rs.days < 0 || rs.days > 127) {
                throw new IllegalArgumentException(label + ": days must be 0..127");
            }
            if (rs.dayOfMonth < 1 || rs.dayOfMonth > 31) {
                throw new IllegalArgumentException(label + ": day_of_month must be 1..31");
            }
            if (rs.month < 1 || rs.month > 12) {
                throw new IllegalArgumentException(label + ": month must be 1..12");
            }
            if (rs.channelMask < 0 || rs.channelMask > 0x0FFF) {
                throw new IllegalArgumentException(label + ": channel_mask must be 0..4095");
            }
            if (rs.from == null || rs.to == null
                    || !TIME.matcher(rs.from).matches() || !TIME.matcher(rs.to).matches()) {
                throw new IllegalArgumentException(label + ": from/to must use HH:MM");
            }
            if (rs.channels != null) {
                for (Integer c : rs.channels) {
                    if (c == null || c < 1 || c > 12) {
                        throw new IllegalArgumentException(label + ": channel id must be 1..12");
                    }
                    rs.channelMask |= 1 << (c - 1);
                }
            }
        }
        incoming.channels = channels;
        incoming.recurring = recurring;
        return incoming;
    }

    private void checkName(String name, String label) {
        if (name == null || name.isBlank() || name.length() > 23) {
            throw new IllegalArgumentException(label + ": name must be 1..23 characters");
        }
    }

    private void checkMode(String mode, String label) {
        if (!Set.of("auto", "on", "off").contains(mode)) {
            throw new IllegalArgumentException(label + ": mode must be auto/on/off");
        }
    }

    private void checkWindows(List<ScheduleWindow> windows, String label) {
        if (windows == null || windows.size() > 4) {
            throw new IllegalArgumentException(label + ": at most 4 windows are allowed");
        }
        for (ScheduleWindow w : windows) {
            if (w == null || w.on == null || w.off == null
                    || !TIME.matcher(w.on).matches() || !TIME.matcher(w.off).matches()) {
                throw new IllegalArgumentException(label + ": windows must use HH:MM");
            }
        }
    }

    private String validProfile(String profile) {
        return PROFILES.contains(profile) ? profile : "agri";
    }

    private int clampInterval(int seconds) {
        return Math.max(10, Math.min(3600, seconds));
    }

    // ---------------------------------------------------------------------
    // Snapshots
    // ---------------------------------------------------------------------
    public synchronized ObjectNode snapshot() {
        ObjectNode out = mapper.createObjectNode();
        out.put("serverTime", ISO.format(Instant.now()));
        out.put("serverTimezone", serverTimezone);
        out.put("tokenRequired", !configuredToken.isBlank());
        ArrayNode arr = out.putArray("devices");
        int confirmed = 0;
        int online = 0;
        for (Device d : devices.values()) {
            arr.add(summary(d));
            if (d.confirmed) {
                confirmed++;
            }
            if (isOnline(d)) {
                online++;
            }
        }
        out.put("deviceCount", devices.size());
        out.put("confirmedCount", confirmed);
        out.put("onlineCount", online);
        return out;
    }

    public synchronized ObjectNode snapshot(String id) {
        Device dev = requireDevice(id);
        ObjectNode out = mapper.createObjectNode();
        out.put("serverTime", ISO.format(Instant.now()));
        out.put("serverTimezone", serverTimezone);
        out.put("tokenRequired", !configuredToken.isBlank());
        out.setAll(summary(dev));

        ObjectNode queueNode = out.putObject("request");
        queueNode.put("queued", dev.queue.size());
        queueNode.put("inflight", dev.inflight != null);
        queueNode.put("inflightId", dev.inflight == null ? "" : dev.inflight.id);
        queueNode.put("inflightType", dev.inflight == null ? ""
                : dev.inflight.type.name().toLowerCase(Locale.ROOT));
        queueNode.put("inflightAttempts", dev.inflight == null ? 0 : dev.inflight.attempts);
        queueNode.set("lastAck", dev.lastAck == null ? mapper.nullNode() : dev.lastAck.deepCopy());

        ArrayNode q = queueNode.putArray("items");
        dev.queue.forEach(r -> {
            ObjectNode item = q.addObject();
            item.put("id", r.id);
            item.put("type", r.type.name().toLowerCase(Locale.ROOT));
            item.put("attempts", r.attempts);
            item.put("created", ISO.format(r.createdAt));
        });

        out.set("config", mapper.valueToTree(dev.config));
        out.set("relayMap", mapper.valueToTree(RelayDefinition.esp32Defaults()));
        out.set("lastData", dev.lastData == null ? mapper.nullNode() : dev.lastData.deepCopy());
        out.set("ackHistory", mapper.valueToTree(new ArrayList<>(dev.ackHistory)));
        out.set("events", mapper.valueToTree(new ArrayList<>(dev.events)));
        return out;
    }

    private ObjectNode summary(Device dev) {
        ObjectNode n = mapper.createObjectNode();
        n.put("id", dev.id);
        n.put("name", dev.name == null || dev.name.isBlank() ? dev.id : dev.name);
        n.put("firmware", dev.firmware);
        n.put("profile", dev.profile);
        n.put("confirmed", dev.confirmed);
        n.put("rejected", dev.rejected);
        n.put("remoteUser", dev.remoteUser);
        n.put("pollIntervalSec", dev.pollIntervalSec);
        n.put("online", isOnline(dev));
        n.put("pollCount", dev.pollCount);
        n.put("firstSeen", ISO.format(dev.firstSeen));
        n.put("lastPoll", dev.lastPollAt == null ? "" : ISO.format(dev.lastPollAt));
        n.put("lastPollEpoch", dev.lastPollAt == null ? 0 : dev.lastPollAt.toEpochMilli());
        n.put("secondsSincePoll", dev.lastPollAt == null ? -1
                : Math.max(0, Instant.now().getEpochSecond() - dev.lastPollAt.getEpochSecond()));
        n.set("lastData", dev.lastData == null ? mapper.nullNode() : dev.lastData.deepCopy());
        n.put("queued", dev.queue.size());
        n.put("inflight", dev.inflight != null);
        return n;
    }

    public synchronized boolean isOnline() {
        for (Device d : devices.values()) {
            if (isOnline(d)) {
                return true;
            }
        }
        return false;
    }

    public synchronized boolean isOnline(String id) {
        Device d = devices.get(id);
        return d != null && isOnline(d);
    }

    private boolean isOnline(Device dev) {
        return dev.lastPollAt != null
                && Instant.now().getEpochSecond() - dev.lastPollAt.getEpochSecond() <= 90;
    }

    // ---------------------------------------------------------------------
    // Device-user lookup (used by Spring Security)
    // ---------------------------------------------------------------------
    public synchronized DeviceUser findDeviceUser(String username) {
        for (Device d : devices.values()) {
            if (d.confirmed && username != null && username.equals(d.remoteUser)
                    && !d.passwordHash.isBlank()) {
                return new DeviceUser(d.id, d.passwordHash);
            }
        }
        return null;
    }

    public synchronized String findDeviceIdByUser(String username) {
        DeviceUser user = findDeviceUser(username);
        return user == null ? null : user.deviceId();
    }

    public record DeviceUser(String deviceId, String passwordHash) {
    }

    // ---------------------------------------------------------------------
    // Internals
    // ---------------------------------------------------------------------
    private Device requireDevice(String id) {
        Device dev = devices.get(id);
        if (dev == null) {
            throw new IllegalArgumentException("unknown device: " + id);
        }
        return dev;
    }

    private ObjectNode requestSummary(Device dev, String status, RemoteRequest r) {
        ObjectNode out = mapper.createObjectNode();
        out.put("ok", true);
        out.put("status", status);
        out.put("id", r.id);
        out.put("type", r.type.name().toLowerCase(Locale.ROOT));
        out.put("deviceId", dev.id);
        out.put("queued", dev.queue.size());
        out.put("inflight", dev.inflight != null);
        return out;
    }

    private void addEvent(Device dev, String type, String message) {
        ObjectNode e = mapper.createObjectNode();
        e.put("at", ISO.format(Instant.now()));
        e.put("type", type);
        e.put("message", message);
        dev.events.addFirst(e);
        while (dev.events.size() > MAX_EVENTS) {
            dev.events.removeLast();
        }
    }

    private enum RequestType {CONTROL, DATA, CONFIG}

    private static final class RemoteRequest {
        final String id;
        final RequestType type;
        final ObjectNode payload;
        Instant createdAt = Instant.now();
        int attempts;
        String result = "";

        RemoteRequest(String id, RequestType type, ObjectNode payload) {
            this.id = id;
            this.type = type;
            this.payload = payload;
        }

        ObjectNode toResponse(ObjectMapper mapper) {
            ObjectNode response = mapper.createObjectNode();
            response.put("request", type.name().toLowerCase(Locale.ROOT));
            response.put("id", id);
            if (type == RequestType.CONTROL) {
                response.set("commands", payload.get("commands"));
            } else if (type == RequestType.CONFIG) {
                response.setAll(payload);
            }
            return response;
        }
    }

    private static final class Device {
        final String id;
        String name = "";
        String firmware = "";
        String profile = "agri";
        boolean confirmed;
        boolean rejected;
        String remoteUser = "";
        String passwordHash = "";
        int pollIntervalSec = 60;
        Instant firstSeen = Instant.now();
        Instant lastPollAt;
        long pollCount;
        long requestSeq;
        ControllerConfig config = ControllerConfig.demoDefaults();
        JsonNode lastData;
        JsonNode lastAck;
        final Deque<RemoteRequest> queue = new ArrayDeque<>();
        final Deque<ObjectNode> ackHistory = new ArrayDeque<>();
        final Deque<ObjectNode> events = new ArrayDeque<>();
        RemoteRequest inflight;

        Device(String id) {
            this.id = id;
        }
    }
}
