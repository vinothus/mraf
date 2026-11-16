package com.mraf.server.service;

import com.mraf.server.db.ServerSettingsEntity;
import com.mraf.server.db.ServerSettingsRepository;
import org.springframework.stereotype.Service;
import org.springframework.transaction.annotation.Transactional;

/**
 * Branding values for the remote server UI. The heading and subheading are
 * global (not per device) and are returned by the public /api/branding
 * endpoint so the sign-in card and the app header can render them before the
 * user authenticates.
 */
@Service
public class BrandingService {
    public static final String DEFAULT_HEADING = "நீர்காவலன் Neer Kaavalan";
    public static final String DEFAULT_SUBHEADING =
            "நுண் வேளாண்மை கட்டுப்பாட்டு மையம் – Smart Agriculture Control Center";

    private static final String ROW_ID = "branding";
    private static final int MAX_HEADING = 120;
    private static final int MAX_SUBHEADING = 200;

    private final ServerSettingsRepository repo;

    public BrandingService(ServerSettingsRepository repo) {
        this.repo = repo;
    }

    @Transactional(readOnly = true)
    public Branding get() {
        return repo.findById(ROW_ID).map(BrandingService::resolve).orElseGet(BrandingService::defaults);
    }

    @Transactional
    public Branding update(String heading, String subheading) {
        ServerSettingsEntity row = repo.findById(ROW_ID)
                .orElseGet(() -> new ServerSettingsEntity(ROW_ID));
        row.heading = trim(heading, MAX_HEADING);
        row.subheading = trim(subheading, MAX_SUBHEADING);
        repo.save(row);
        return resolve(row);
    }

    private static Branding resolve(ServerSettingsEntity row) {
        String heading = row.heading == null ? "" : row.heading.trim();
        String subheading = row.subheading == null ? "" : row.subheading.trim();
        return new Branding(
                heading.isEmpty() ? DEFAULT_HEADING : heading,
                subheading.isEmpty() ? DEFAULT_SUBHEADING : subheading);
    }

    private static Branding defaults() {
        return new Branding(DEFAULT_HEADING, DEFAULT_SUBHEADING);
    }

    private static String trim(String value, int max) {
        if (value == null) {
            return "";
        }
        String trimmed = value.trim();
        return trimmed.length() > max ? trimmed.substring(0, max) : trimmed;
    }

    public record Branding(String heading, String subheading) {
    }
}
