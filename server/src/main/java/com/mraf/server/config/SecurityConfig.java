package com.mraf.server.config;

import com.mraf.server.service.RemoteStateService;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;
import org.springframework.security.config.Customizer;
import org.springframework.security.config.annotation.web.builders.HttpSecurity;
import org.springframework.security.config.annotation.web.configurers.AbstractHttpConfigurer;
import org.springframework.security.core.userdetails.User;
import org.springframework.security.core.userdetails.UserDetailsService;
import org.springframework.security.core.userdetails.UsernameNotFoundException;
import org.springframework.security.crypto.factory.PasswordEncoderFactories;
import org.springframework.security.crypto.password.PasswordEncoder;
import org.springframework.security.web.SecurityFilterChain;

@Configuration
public class SecurityConfig {
    @Bean
    SecurityFilterChain securityFilterChain(HttpSecurity http) throws Exception {
        http
                .csrf(AbstractHttpConfigurer::disable)
                .authorizeHttpRequests(auth -> auth
                        .requestMatchers("/api/health").permitAll()
                        .anyRequest().authenticated())
                .httpBasic(Customizer.withDefaults());
        return http.build();
    }

    /**
     * Admin users come from configuration; device users are created when an
     * ESP32 registers its server login and an admin confirms the device.
     */
    @Bean
    UserDetailsService users(
            RemoteStateService state,
            PasswordEncoder encoder,
            @Value("${mraf.dashboard.user:admin}") String adminUser,
            @Value("${mraf.dashboard.password:mraf-demo}") String adminPassword) {
        String adminHash = encoder.encode(adminPassword);
        return username -> {
            if (adminUser.equals(username)) {
                return User.withUsername(username)
                        .password(adminHash)
                        .roles("ADMIN")
                        .build();
            }
            RemoteStateService.DeviceUser deviceUser = state.findDeviceUser(username);
            if (deviceUser != null) {
                return User.withUsername(username)
                        .password(deviceUser.passwordHash())
                        .roles("DEVICE")
                        .build();
            }
            throw new UsernameNotFoundException(username);
        };
    }

    @Bean
    PasswordEncoder passwordEncoder() {
        return PasswordEncoderFactories.createDelegatingPasswordEncoder();
    }
}
