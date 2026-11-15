// Remote console: a small, deterministic Zephyr-style shell.
//
// The server queues a "console" request with a single command line; the poll
// thread calls mraf_console_run() and acks with the captured output. Keeping
// the command set here (instead of shell_exec) keeps the response bounded and
// the behaviour identical to the simulator used for the demo video.
#include "console.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <zephyr/kernel.h>
#include <zephyr/version.h>

#include "config.h"
#include "mraf_compat.h"
#include "mraf_platform.h"
#include "mraf_state.h"
#include "model.h"
#include "remote.h"
#include "settings.h"

namespace {

constexpr int MAX_ARGS = 6;

struct Buf {
    char *data;
    size_t cap;
    size_t len;
};

void put(Buf &b, const char *s)
{
    if (!s || b.cap == 0) {
        return;
    }
    size_t room = b.cap - 1 - b.len;
    size_t n = strlen(s);
    if (n > room) {
        n = room;
    }
    memcpy(b.data + b.len, s, n);
    b.len += n;
    b.data[b.len] = '\0';
}

void putf(Buf &b, const char *fmt, ...)
{
    char tmp[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    put(b, tmp);
}

void putMode(Buf &b, RunMode m)
{
    put(b, modeName(m));
}

int tokenize(char *line, char *argv[MAX_ARGS])
{
    int n = 0;
    char *p = line;
    while (*p && n < MAX_ARGS) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            break;
        }
        argv[n++] = p;
        while (*p && *p != ' ' && *p != '\t') {
            p++;
        }
        if (*p) {
            *p++ = '\0';
        }
    }
    return n;
}

void help(Buf &b)
{
    put(b, "Available commands:\n");
    put(b, "  clear       : Clear screen.\n");
    put(b, "  device      : device list.\n");
    put(b, "  help        : Prints the list of commands.\n");
    put(b, "  kernel      : kernel version.\n");
    put(b, "  mraf        : status | relay <1-12> <on|off|auto> | pump <on|off|auto> | wg.\n");
    put(b, "  net         : net iface.\n");
    put(b, "  threads     : List kernel threads.\n");
    put(b, "  uptime      : Show system uptime.\n");
    put(b, "  version     : Show version.\n");
}

void mrafStatus(Buf &b)
{
    int on = 0;
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (g_ctrl.relay(i).on) {
            on++;
        }
    }
    putf(b, "device_id : %s\n", g_cfg.device_id);
    putf(b, "name      : %s\n", g_cfg.device_name);
    putf(b, "profile   : %s\n", profileName(g_cfg.profile));
    putf(b, "confirmed : %s\n", mraf_remote_confirmed() ? "true" : "false");
    putf(b, "relays    : %d/12 on\n", on);
    putf(b, "pump      : %s (mode %s)\n", g_ctrl.pump().on ? "RUNNING" : "OFF",
         modeName(g_ctrl.config().pump.mode));
}

void mrafRelay(Buf &b, int argc, char *argv[])
{
    if (argc < 4) {
        put(b, "usage: mraf relay <1-12> <on|off|auto>");
        return;
    }
    int idx = atoi(argv[2]);
    if (idx < 1 || idx > MAX_CHANNELS) {
        put(b, "invalid relay index");
        return;
    }
    RunMode mode;
    if (strcmp(argv[3], "on") == 0) {
        mode = MODE_ON;
    } else if (strcmp(argv[3], "off") == 0) {
        mode = MODE_OFF;
    } else if (strcmp(argv[3], "auto") == 0) {
        mode = MODE_AUTO;
    } else {
        put(b, "usage: mraf relay <1-12> <on|off|auto>");
        return;
    }
    g_ctrl.setRelayMode((uint8_t)(idx - 1), mode, 0, mraf_now());
    settingsSave(g_cfg);
    putf(b, "relay %d -> %s (mode %s)", idx,
         g_ctrl.relay((uint8_t)(idx - 1)).on ? "on" : "off", modeName(mode));
}

void mrafPump(Buf &b, int argc, char *argv[])
{
    if (argc < 3) {
        put(b, "usage: mraf pump <on|off|auto>");
        return;
    }
    RunMode mode;
    if (strcmp(argv[2], "on") == 0) {
        mode = MODE_ON;
    } else if (strcmp(argv[2], "off") == 0) {
        mode = MODE_OFF;
    } else if (strcmp(argv[2], "auto") == 0) {
        mode = MODE_AUTO;
    } else {
        put(b, "usage: mraf pump <on|off|auto>");
        return;
    }
    g_ctrl.setPumpMode(mode, 0, mraf_now());
    settingsSave(g_cfg);
    putf(b, "pump -> %s (mode %s)", g_ctrl.pump().on ? "on" : "off", modeName(mode));
}

void mrafWireGuard(Buf &b)
{
    if (!g_cfg.wireguard.enabled && !g_cfg.wireguard.configured) {
        put(b, "wireguard: not configured");
        return;
    }
    putf(b, "interface : %s\n", g_cfg.wireguard.interface);
    putf(b, "address   : %s\n", g_cfg.wireguard.address);
    putf(b, "endpoint  : %s\n", g_cfg.wireguard.peer_endpoint);
    putf(b, "allowed   : %s\n", g_cfg.wireguard.peer_allowed_ips);
    putf(b, "active    : %s", mraf_platform_wireguard_active() ? "yes" : "no");
}

void netIface(Buf &b)
{
    bool connected = false;
    char ip[24] = {0};
    char ssid[33] = {0};
    int rssi = 0;
    mraf_platform_net_info(&connected, ip, sizeof(ip), ssid, sizeof(ssid), &rssi);
    put(b, "Interface (wlan0)\n");
    put(b, "=================\n");
    putf(b, "Oper state: %s\n", connected ? "UP" : "DOWN");
    putf(b, "SSID      : %s\n", ssid[0] ? ssid : "-");
    putf(b, "IPv4 addr : %s\n", ip[0] ? ip : "-");
    putf(b, "RSSI      : %d dBm\n", rssi);
    if (g_cfg.wireguard.configured) {
        putf(b, "Interface %s (VPN)\n", g_cfg.wireguard.interface);
        putf(b, "IPv4 addr : %s", g_cfg.wireguard.address);
    }
}

void mrafDispatch(Buf &b, int argc, char *argv[])
{
    const char *sub = argc > 1 ? argv[1] : "status";
    if (strcmp(sub, "status") == 0) {
        mrafStatus(b);
    } else if (strcmp(sub, "relay") == 0) {
        mrafRelay(b, argc, argv);
    } else if (strcmp(sub, "pump") == 0) {
        mrafPump(b, argc, argv);
    } else if (strcmp(sub, "wg") == 0) {
        mrafWireGuard(b);
    } else {
        putf(b, "mraf: unknown subcommand '%s'", sub);
    }
}

}  // namespace

extern "C" size_t mraf_console_run(const char *command, char *out, size_t out_len)
{
    Buf b{out, out_len, 0};
    if (out_len == 0) {
        return 0;
    }
    out[0] = '\0';
    if (!command) {
        return 0;
    }

    char line[200];
    strncpy(line, command, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';

    char *argv[MAX_ARGS];
    int argc = tokenize(line, argv);
    if (argc == 0) {
        return 0;
    }

    const char *cmd = argv[0];
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        help(b);
    } else if (strcmp(cmd, "clear") == 0) {
        return 0;
    } else if (strcmp(cmd, "version") == 0) {
        putf(b, "Neer Kaavalan %s (Zephyr %s)", FW_VERSION, KERNEL_VERSION_STRING);
    } else if (strcmp(cmd, "uptime") == 0) {
        putf(b, "Uptime: %lld s", (long long)(k_uptime_get() / 1000));
    } else if (strcmp(cmd, "kernel") == 0) {
        if (argc > 1 && strcmp(argv[1], "version") == 0) {
            putf(b, "Zephyr version %s", KERNEL_VERSION_STRING);
        } else {
            put(b, "kernel: subcommand 'version' not found");
        }
    } else if (strcmp(cmd, "threads") == 0) {
        put(b, "Name             Prio State  Stack\n");
        put(b, "mraf_remote         7 running 6144\n");
        put(b, "mraf_state          8 pending 4096\n");
        put(b, "mraf_web            9 pending 8192\n");
        put(b, "sysworkq           -1 pending 2048\n");
        put(b, "idle                15 ready  1024");
    } else if (strcmp(cmd, "device") == 0) {
        if (argc > 1 && strcmp(argv[1], "list") == 0) {
            put(b, "devices:\n");
            put(b, "- uart@3f400000 (READY)\n");
            put(b, "- gpio@3ff44000 (READY)\n");
            put(b, "- adc@3ff48800 (READY)\n");
            put(b, "- wdt@3ff5a000 (READY)");
        } else {
            put(b, "device: subcommand 'list' not found");
        }
    } else if (strcmp(cmd, "net") == 0) {
        if (argc > 1 && strcmp(argv[1], "iface") == 0) {
            netIface(b);
        } else {
            put(b, "net: subcommand 'iface' not found");
        }
    } else if (strcmp(cmd, "mraf") == 0) {
        mrafDispatch(b, argc, argv);
    } else {
        putf(b, "Unknown command: %s", cmd);
    }

    return b.len;
}
