// Minimal CoAP confirmable POST client using Zephyr sockets.
#include "coap_client.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>

static uint16_t s_msg_id;
static uint32_t s_token_seq;

static bool appendOption(uint8_t *buf, size_t cap, size_t &pos,
                         uint16_t &last, uint16_t number,
                         const uint8_t *value, size_t len)
{
    if (number < last) {
        return false;
    }
    uint16_t delta = number - last;
    uint8_t dn;
    size_t extra = 0;
    if (delta < 13) {
        dn = (uint8_t)delta;
    } else if (delta < 269) {
        dn = 13;
        extra += 1;
    } else {
        dn = 14;
        extra += 2;
    }

    uint8_t ln;
    if (len < 13) {
        ln = (uint8_t)len;
    } else if (len < 269) {
        ln = 13;
        extra += 1;
    } else {
        ln = 14;
        extra += 2;
    }

    if (pos + 1 + extra + len > cap) {
        return false;
    }
    buf[pos++] = (uint8_t)((dn << 4) | ln);
    if (delta >= 13) {
        uint16_t d = (uint16_t)(delta - (dn == 13 ? 13 : 269));
        if (dn == 13) {
            buf[pos++] = (uint8_t)d;
        } else {
            buf[pos++] = (uint8_t)(d >> 8);
            buf[pos++] = (uint8_t)d;
        }
    }
    if (len >= 13) {
        uint16_t l = (uint16_t)(len - (ln == 13 ? 13 : 269));
        if (ln == 13) {
            buf[pos++] = (uint8_t)l;
        } else {
            buf[pos++] = (uint8_t)(l >> 8);
            buf[pos++] = (uint8_t)l;
        }
    }
    if (len) {
        memcpy(buf + pos, value, len);
        pos += len;
    }
    last = number;
    return true;
}

static int buildRequest(const char *path, const char *payload,
                        uint8_t *buf, size_t cap)
{
    if (!path || !payload || cap < 16) {
        return -1;
    }

    s_msg_id++;
    if (s_msg_id == 0) {
        s_msg_id = 1;
    }
    uint32_t token = (++s_token_seq) * 2654435761u;

    buf[0] = 0x44; // version 1, CON, 4-byte token
    buf[1] = 2;    // POST
    buf[2] = (uint8_t)(s_msg_id >> 8);
    buf[3] = (uint8_t)s_msg_id;
    memcpy(buf + 4, &token, 4);

    size_t pos = 8;
    uint16_t last = 0;
    const char *p = path;
    while (*p == '/') {
        p++;
    }
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t seg = slash ? (size_t)(slash - p) : strlen(p);
        if (seg && !appendOption(buf, cap, pos, last, 11, (const uint8_t *)p, seg)) {
            return -1;
        }
        if (!slash) {
            break;
        }
        p = slash + 1;
        while (*p == '/') {
            p++;
        }
    }

    uint8_t content_format = 50; // application/json
    if (!appendOption(buf, cap, pos, last, 12, &content_format, 1)) {
        return -1;
    }

    size_t plen = strlen(payload);
    if (pos + 1 + plen > cap) {
        return -1;
    }
    buf[pos++] = 0xFF;
    memcpy(buf + pos, payload, plen);
    pos += plen;
    return (int)pos;
}

static int readPayload(const uint8_t *in, int n, size_t pos,
                       char *response, size_t response_cap)
{
    if (!response || response_cap == 0) {
        return -1;
    }
    response[0] = '\0';
    while (pos < (size_t)n) {
        uint8_t b = in[pos++];
        if (b == 0xFF) {
            size_t plen = (size_t)n - pos;
            if (plen >= response_cap) {
                plen = response_cap - 1;
            }
            memcpy(response, in + pos, plen);
            response[plen] = '\0';
            return 0;
        }
        uint8_t d = (uint8_t)(b >> 4);
        uint8_t l = (uint8_t)(b & 0x0F);
        if (d == 13) {
            pos += 1;
        } else if (d == 14) {
            pos += 2;
        } else if (d == 15) {
            return -1;
        }
        if (l == 13) {
            pos += 1;
        } else if (l == 14) {
            pos += 2;
        } else if (l == 15) {
            return -1;
        }
        if (pos > (size_t)n) {
            return -1;
        }
        pos += l;
        if (pos > (size_t)n) {
            return -1;
        }
    }
    return 0;
}

int coapPost(const char *host, uint16_t port, const char *path,
             const char *payload, char *response, size_t response_cap,
             unsigned long timeout_ms)
{
    if (!host || !host[0] || !path || !payload) {
        return -1;
    }
    if (response && response_cap) {
        response[0] = '\0';
    }

    struct zsock_addrinfo hints;
    struct zsock_addrinfo *res = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (zsock_getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) {
        return -3;
    }

    struct sockaddr_in addr;
    memcpy(&addr, res->ai_addr, sizeof(addr));
    addr.sin_port = htons(port);
    zsock_freeaddrinfo(res);

    uint8_t packet[640];
    int plen = buildRequest(path, payload, packet, sizeof(packet));
    if (plen < 0) {
        return -1;
    }

    int fd = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return -4;
    }
    if (zsock_connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        zsock_close(fd);
        return -4;
    }

    const uint8_t *token = packet + 4;
    uint16_t mid = (uint16_t)((packet[2] << 8) | packet[3]);
    static uint8_t in[4096];

    int result = -2;
    for (uint8_t attempt = 0; attempt < 2 && result == -2; attempt++) {
        if (zsock_send(fd, packet, (size_t)plen, 0) < 0) {
            result = -5;
            break;
        }
        int64_t start = k_uptime_get();
        while (k_uptime_get() - start < (int64_t)timeout_ms) {
            struct zsock_pollfd pfd = {.fd = fd, .events = ZSOCK_POLLIN};
            int pr = zsock_poll(&pfd, 1, 50);
            if (pr <= 0) {
                continue;
            }
            int got = zsock_recv(fd, in, sizeof(in), 0);
            if (got < 8) {
                continue;
            }
            uint8_t tkl = (uint8_t)(in[0] & 0x0F);
            uint16_t rmid = (uint16_t)((in[2] << 8) | in[3]);
            if ((in[0] >> 6) == 1 && tkl == 4 &&
                memcmp(in + 4, token, 4) == 0 && rmid == mid) {
                uint8_t code = in[1];
                if (code == 0) {
                    continue; // empty ACK, wait for the separate response
                }
                readPayload(in, got, (size_t)(4 + tkl), response, response_cap);
                result = code;
                break;
            }
        }
    }

    zsock_close(fd);
    return result;
}
