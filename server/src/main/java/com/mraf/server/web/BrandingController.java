package com.mraf.server.web;

import com.mraf.server.service.BrandingService;
import org.springframework.http.HttpStatus;
import org.springframework.security.core.Authentication;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PutMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RestController;
import org.springframework.web.server.ResponseStatusException;

import java.util.Map;

@RestController
@RequestMapping("/api")
public class BrandingController {
    private final BrandingService branding;

    public BrandingController(BrandingService branding) {
        this.branding = branding;
    }

    /** Public: the sign-in card and header read this before authentication. */
    @GetMapping("/branding")
    public BrandingService.Branding get() {
        return branding.get();
    }

    @PutMapping("/branding")
    public BrandingService.Branding update(@RequestBody(required = false) Map<String, String> body,
                                           Authentication auth) {
        requireAdmin(auth);
        String heading = body == null ? null : body.get("heading");
        String subheading = body == null ? null : body.get("subheading");
        return branding.update(heading, subheading);
    }

    private static void requireAdmin(Authentication auth) {
        boolean admin = auth != null && auth.getAuthorities().stream()
                .anyMatch(a -> "ROLE_ADMIN".equals(a.getAuthority()));
        if (!admin) {
            throw new ResponseStatusException(HttpStatus.FORBIDDEN, "admin access required");
        }
    }
}
