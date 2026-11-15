package com.mraf.server.wireguard;

import org.springframework.boot.context.properties.ConfigurationProperties;
import org.springframework.stereotype.Component;

/**
 * Configuration for the server side of the WireGuard overlay that connects
 * ESP32 controllers to this Spring Boot control plane.
 */
@Component
@ConfigurationProperties(prefix = "mraf.wireguard")
public class WireGuardProperties {
    /** Enable the WireGuard control plane. */
    private boolean enabled = true;

    /** Kernel interface name used when the host tools are managed. */
    private String interfaceName = "wg0";

    /** Overlay subnet. The server takes the first host address. */
    private String subnet = "10.66.66.0/24";

    /** Server overlay address, must be inside the subnet. */
    private String serverAddress = "10.66.66.1";

    /** UDP port the WireGuard server listens on. */
    private int listenPort = 51820;

    /**
     * Public {@code host:port} clients dial. When blank it is derived from the
     * incoming request host and the listen port.
     */
    private String endpoint = "";

    /** Persistent keepalive in seconds for peers behind NAT. */
    private int persistentKeepalive = 25;

    /** DNS pushed to clients (optional). */
    private String dns = "";

    /** Allowed IPs routed through the tunnel for clients. */
    private String allowedIps = "10.66.66.0/24";

    /**
     * When true the server shells out to {@code wg-quick}/{@code wg} to apply
     * the rendered configuration. Disabled by default so the control plane can
     * run on hosts without the WireGuard tools installed.
     */
    private boolean manageKernel = false;

    /** Directory used to persist the server key and rendered configs. */
    private String configDir = "data/wireguard";

    /** Automatically create a peer and queue its config when a device is confirmed. */
    private boolean autoProvision = true;

    public boolean isEnabled() {
        return enabled;
    }

    public void setEnabled(boolean enabled) {
        this.enabled = enabled;
    }

    public String getInterfaceName() {
        return interfaceName;
    }

    public void setInterfaceName(String interfaceName) {
        this.interfaceName = interfaceName;
    }

    public String getSubnet() {
        return subnet;
    }

    public void setSubnet(String subnet) {
        this.subnet = subnet;
    }

    public String getServerAddress() {
        return serverAddress;
    }

    public void setServerAddress(String serverAddress) {
        this.serverAddress = serverAddress;
    }

    public int getListenPort() {
        return listenPort;
    }

    public void setListenPort(int listenPort) {
        this.listenPort = listenPort;
    }

    public String getEndpoint() {
        return endpoint;
    }

    public void setEndpoint(String endpoint) {
        this.endpoint = endpoint;
    }

    public int getPersistentKeepalive() {
        return persistentKeepalive;
    }

    public void setPersistentKeepalive(int persistentKeepalive) {
        this.persistentKeepalive = persistentKeepalive;
    }

    public String getDns() {
        return dns;
    }

    public void setDns(String dns) {
        this.dns = dns;
    }

    public String getAllowedIps() {
        return allowedIps;
    }

    public void setAllowedIps(String allowedIps) {
        this.allowedIps = allowedIps;
    }

    public boolean isManageKernel() {
        return manageKernel;
    }

    public void setManageKernel(boolean manageKernel) {
        this.manageKernel = manageKernel;
    }

    public String getConfigDir() {
        return configDir;
    }

    public void setConfigDir(String configDir) {
        this.configDir = configDir;
    }

    public boolean isAutoProvision() {
        return autoProvision;
    }

    public void setAutoProvision(boolean autoProvision) {
        this.autoProvision = autoProvision;
    }
}
