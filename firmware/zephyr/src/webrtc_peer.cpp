// Portable DTLS/SCTP data-channel transport (see webrtc_peer.h).
//
// Layout:
//   * ICE-lite: a STUN responder on the same UDP port as DTLS, a one-shot
//     srflx lookup against the configured STUN server, and an optional TURN
//     allocation (RFC 5766) that gives the device a relayed candidate. TURN is
//     used both ways: incoming peer payloads arrive in Data Indications and
//     outgoing payloads are wrapped in Send Indications, all on the same
//     socket. If STUN cannot produce a reachable path the relay candidate is
//     still advertised, which is what makes the link work behind symmetric NAT
//     and on the QEMU slirp network.
//   * DTLS 1.2 server: mbedTLS with an embedded ECDSA P-256 identity. The
//     fingerprint is computed from the DER certificate and advertised in the
//     answer SDP, which is all a browser verifies.
//   * SCTP association: just the subset WebRTC needs (INIT/INIT_ACK,
//     COOKIE_ECHO/COOKIE_ACK, DATA/SACK, HEARTBEAT, DCEP OPEN/ACK). Chunks are
//     carried over DTLS with protocol id 51 (strings) on stream 0, matching
//     the dashboard's "mraf" channel.
//
// All socket access is non-blocking; one thread owns the transport and the
// signalling thread only enqueues one outbound application message at a time.
#include "webrtc_peer.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/posix/fcntl.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <psa/crypto.h>

#include "mraf_platform.h"
#include "mraf_state.h"
#include "webrtc_cert.h"

#ifndef CONFIG_MRAF_WEBRTC_MSG_MAX
#define CONFIG_MRAF_WEBRTC_MSG_MAX 8192
#endif

#define MSG_MAX CONFIG_MRAF_WEBRTC_MSG_MAX

#define PEER_PORT 5000
#define STUN_MAGIC 0x2112A442u
#define STUN_CRC_XOR 0x5354554Eu

// STUN/TURN message types used by the relay client.
#define STUN_BINDING_REQ 0x0001
#define TURN_ALLOCATE_REQ 0x0003
#define TURN_REFRESH_REQ 0x0004
#define TURN_SEND_IND 0x0016
#define TURN_DATA_IND 0x0017
#define TURN_PERM_REQ 0x0008
#define TURN_ALLOCATE_OK 0x0103
#define TURN_REFRESH_OK 0x0104
#define TURN_PERM_OK 0x0108
#define TURN_ERROR 0x0113
// Attributes.
#define STUN_ATTR_USERNAME 0x0006
#define STUN_ATTR_MESSAGE_INTEGRITY 0x0008
#define STUN_ATTR_ERROR_CODE 0x0009
#define STUN_ATTR_LIFETIME 0x000D
#define STUN_ATTR_XOR_PEER 0x0012
#define STUN_ATTR_DATA 0x0013
#define STUN_ATTR_REALM 0x0014
#define STUN_ATTR_NONCE 0x0015
#define STUN_ATTR_XOR_RELAYED 0x0016
#define STUN_ATTR_REQUESTED_TRANSPORT 0x0019
#define STUN_ATTR_XOR_MAPPED 0x0020
#define STUN_ATTR_FINGERPRINT 0x8028
#define TURN_TRANSPORT_UDP 17
#define TURN_LIFETIME 600
#define TURN_TIMEOUT_MS 700
#define TURN_PERM_MAX 6
#define TURN_CAND_MAX 4

#define SCTP_PKT_MAX 1100
#define SCTP_DATA_MAX 1061
#define FRAG_MAX ((MSG_MAX + SCTP_DATA_MAX - 1) / SCTP_DATA_MAX)
#define SCTP_RX_MAX 4096
#define RX_FRAG_MAX ((SCTP_RX_MAX + SCTP_DATA_MAX - 1) / SCTP_DATA_MAX)
#define STREAM_MAX 8
#define GAP_MAX 16

#define DCEP_PPID 50
#define STRING_PPID 51
#define BINARY_PPID 53

#define SCTP_DATA 0
#define SCTP_INIT 1
#define SCTP_INIT_ACK 2
#define SCTP_SACK 3
#define SCTP_HEARTBEAT 4
#define SCTP_HEARTBEAT_ACK 5
#define SCTP_ABORT 6
#define SCTP_SHUTDOWN 7
#define SCTP_SHUTDOWN_ACK 8
#define SCTP_ERROR 9
#define SCTP_COOKIE_ECHO 10
#define SCTP_COOKIE_ACK 11

#define F_BIT 0x02
#define E_BIT 0x01
#define U_BIT 0x04

typedef void (*rx_cb_t)(const char *message);

struct Peer {
    int fd;
    bool bound;
    struct sockaddr_in cur;
    bool have_cur;
    bool remote_validated;

    char local_ufrag[9];
    char local_pwd[25];
    char remote_ufrag[32];
    char remote_pwd[96];
    char remote_fp[128];
    char host_ip[32];
    char srflx_ip[32];
    uint16_t srflx_port;
    bool have_srflx;

    // TURN relay (RFC 5766). The allocation shares the peer UDP socket, so the
    // relay address is simply another ICE candidate in the answer SDP.
    bool turn_allocated;
    bool have_relay;
    bool relay_path;                    // current path goes through the relay
    struct sockaddr_in turn_srv;
    struct sockaddr_in relay_addr;
    char turn_url[96];
    char turn_user[64];
    char turn_pass[64];
    char turn_realm[96];
    uint8_t turn_nonce[128];
    uint16_t turn_nonce_len;
    uint8_t turn_key[16];
    char relay_ip[32];
    uint16_t relay_port;
    char turn_error[48];
    int64_t turn_refresh_at;
    int64_t turn_perm_at;
    struct sockaddr_in turn_cands[TURN_CAND_MAX];
    uint8_t turn_cand_head;
    uint8_t turn_cand_count;
    struct sockaddr_in turn_perms[TURN_CAND_MAX];
    uint8_t turn_perm_count;
    struct k_mutex turn_lock;
    volatile bool relay_used;           // at least one Data Indication seen

    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt cert;
    mbedtls_pk_context key;
    bool tls_setup;
    volatile bool tls_ready;

    uint32_t timer_int_ms;
    uint32_t timer_fin_ms;
    int64_t timer_int_start;
    int64_t timer_fin_start;

    bool sctp_ready;
    bool dcep_open;
    bool dcep_acked;
    uint16_t dcep_stream;
    uint32_t local_tag;
    uint32_t remote_tag;
    uint32_t local_tsn;
    uint32_t peer_cum_tsn;
    uint16_t remote_port;
    uint16_t out_ssn[STREAM_MAX];

    uint8_t tx[MSG_MAX];
    size_t tx_len;
    uint32_t tx_base;
    uint16_t tx_frags;
    uint16_t tx_done;
    uint16_t tx_ssn;
    bool tx_busy;
    int64_t tx_last;
    uint32_t tx_rto;
    uint8_t tx_attempts;

    uint8_t rx[SCTP_RX_MAX];
    uint32_t rx_base;
    uint32_t rx_seen;
    uint8_t rx_expected;
    size_t rx_len;
    bool rx_active;
    uint16_t rx_stream;
    uint16_t rx_ssn;
    uint8_t rx_ppid;

    uint32_t gaps[GAP_MAX];
    uint8_t gap_count;
    bool sack_pending;
    int64_t sack_at;

    volatile bool stop;
    volatile bool exited;
    rx_cb_t rx_cb;
    struct k_sem wake;
    struct k_sem exited_sem;
    struct k_mutex tx_lock;
};

static struct Peer g_peer;
static struct k_thread g_peer_thread;
K_THREAD_STACK_DEFINE(g_peer_stack, 12288);

// --- small helpers ----------------------------------------------------------

static inline uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static inline void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline uint32_t rd32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline void wr32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t g_rng;

static void rng_seed(void)
{
    uint32_t seed = 0;
    sys_rand_get(&seed, sizeof(seed));
    g_rng = seed ? seed : (uint32_t)k_cycle_get_32() | 1u;
}

static uint32_t rng_next(void)
{
    uint32_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng = x;
    return x;
}

static uint32_t fnv32(const uint8_t *d, size_t n, uint32_t h)
{
    for (size_t i = 0; i < n; i++) {
        h ^= d[i];
        h *= 16777619u;
    }
    return h;
}

// --- SHA-1 / HMAC-SHA1 (STUN MESSAGE-INTEGRITY) -----------------------------

struct Sha1 {
    uint32_t h[5];
    uint64_t len;
    uint8_t buf[64];
    size_t fill;
};

static uint32_t rol32(uint32_t v, int n)
{
    return (v << n) | (v >> (32 - n));
}

static void sha1_block(struct Sha1 *s, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = rd32(p + 4 * i);
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol32(b, 30);
        b = a;
        a = t;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
}

static void sha1_init(struct Sha1 *s)
{
    s->h[0] = 0x67452301u;
    s->h[1] = 0xEFCDAB89u;
    s->h[2] = 0x98BADCFEu;
    s->h[3] = 0x10325476u;
    s->h[4] = 0xC3D2E1F0u;
    s->len = 0;
    s->fill = 0;
}

static void sha1_update(struct Sha1 *s, const uint8_t *d, size_t n)
{
    s->len += n;
    while (n) {
        size_t take = 64 - s->fill;
        if (take > n) {
            take = n;
        }
        memcpy(s->buf + s->fill, d, take);
        s->fill += take;
        d += take;
        n -= take;
        if (s->fill == 64) {
            sha1_block(s, s->buf);
            s->fill = 0;
        }
    }
}

static void sha1_final(struct Sha1 *s, uint8_t out[20])
{
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    sha1_update(s, &pad, 1);
    uint8_t zero = 0;
    while (s->fill != 56) {
        sha1_update(s, &zero, 1);
    }
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) {
        lenb[7 - i] = (uint8_t)(bits >> (8 * i));
    }
    sha1_update(s, lenb, 8);
    for (int i = 0; i < 5; i++) {
        wr32(out + 4 * i, s->h[i]);
    }
}

static void hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *d,
                      size_t n, uint8_t out[20])
{
    uint8_t k[64];
    memset(k, 0, sizeof(k));
    if (key_len > 64) {
        struct Sha1 s;
        sha1_init(&s);
        sha1_update(&s, key, key_len);
        sha1_final(&s, k);
    } else {
        memcpy(k, key, key_len);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    struct Sha1 s;
    sha1_init(&s);
    sha1_update(&s, ipad, 64);
    sha1_update(&s, d, n);
    uint8_t inner[20];
    sha1_final(&s, inner);
    sha1_init(&s);
    sha1_update(&s, opad, 64);
    sha1_update(&s, inner, 20);
    sha1_final(&s, out);
}

// --- MD5 (TURN long-term credential key) ------------------------------------
//
// RFC 5389 derives the long-term credential key as
// MD5(username ":" realm ":" password). mbedTLS' MD5 is not part of the default
// ESP32 PSA profile, so a compact copy lives here; it is only used while a TURN
// allocation is being negotiated.

struct Md5 {
    uint32_t h[4];
    uint64_t len;
    uint8_t buf[64];
    size_t fill;
};

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void wr32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void md5_block(struct Md5 *s, const uint8_t *p)
{
    static const uint32_t k[64] = {
        0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
        0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
        0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
        0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
        0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
        0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
        0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
        0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
        0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
        0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
        0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
        0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
        0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
        0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
        0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
        0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
    };
    static const uint8_t r[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
    };
    uint32_t w[16];
    for (int i = 0; i < 16; i++) {
        w[i] = rd32le(p + 4 * i);
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) & 15;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) & 15;
        }
        uint32_t tmp = d;
        d = c;
        c = b;
        b = b + rol32(a + f + k[i] + w[g], r[i]);
        a = tmp;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
}

static void md5_init(struct Md5 *s)
{
    s->h[0] = 0x67452301u;
    s->h[1] = 0xefcdab89u;
    s->h[2] = 0x98badcfeu;
    s->h[3] = 0x10325476u;
    s->len = 0;
    s->fill = 0;
}

static void md5_update(struct Md5 *s, const void *d, size_t n)
{
    const uint8_t *p = (const uint8_t *)d;
    s->len += n;
    while (n > 0) {
        size_t take = 64 - s->fill;
        if (take > n) {
            take = n;
        }
        memcpy(s->buf + s->fill, p, take);
        s->fill += take;
        p += take;
        n -= take;
        if (s->fill == 64) {
            md5_block(s, s->buf);
            s->fill = 0;
        }
    }
}

static void md5_final(struct Md5 *s, uint8_t out[16])
{
    uint64_t bits = s->len * 8;
    static const uint8_t pad[64] = {0x80};
    size_t padlen = s->fill < 56 ? 56 - s->fill : 120 - s->fill;
    md5_update(s, pad, padlen);
    uint8_t lenb[8];
    wr32le(lenb, (uint32_t)bits);
    wr32le(lenb + 4, (uint32_t)(bits >> 32));
    md5_update(s, lenb, sizeof(lenb));
    for (int i = 0; i < 4; i++) {
        wr32le(out + 4 * i, s->h[i]);
    }
}

static void md5_digest(const void *d, size_t n, uint8_t out[16])
{
    struct Md5 s;
    md5_init(&s);
    md5_update(&s, d, n);
    md5_final(&s, out);
}

// --- SHA-256 (certificate fingerprint) --------------------------------------

struct Sha256 {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t fill;
};

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t ror32(uint32_t v, int n)
{
    return (v >> n) | (v << (32 - n));
}

static void sha256_block(struct Sha256 *s, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = rd32(p + 4 * i);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    uint32_t e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + sha256_k[i] + w[i];
        uint32_t s0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
    s->h[5] += f;
    s->h[6] += g;
    s->h[7] += h;
}

static void sha256_init(struct Sha256 *s)
{
    s->h[0] = 0x6a09e667;
    s->h[1] = 0xbb67ae85;
    s->h[2] = 0x3c6ef372;
    s->h[3] = 0xa54ff53a;
    s->h[4] = 0x510e527f;
    s->h[5] = 0x9b05688c;
    s->h[6] = 0x1f83d9ab;
    s->h[7] = 0x5be0cd19;
    s->len = 0;
    s->fill = 0;
}

static void sha256_update(struct Sha256 *s, const uint8_t *d, size_t n)
{
    s->len += n;
    while (n) {
        size_t take = 64 - s->fill;
        if (take > n) {
            take = n;
        }
        memcpy(s->buf + s->fill, d, take);
        s->fill += take;
        d += take;
        n -= take;
        if (s->fill == 64) {
            sha256_block(s, s->buf);
            s->fill = 0;
        }
    }
}

static void sha256_final(struct Sha256 *s, uint8_t out[32])
{
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    sha256_update(s, &pad, 1);
    uint8_t zero = 0;
    while (s->fill != 56) {
        sha256_update(s, &zero, 1);
    }
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) {
        lenb[7 - i] = (uint8_t)(bits >> (8 * i));
    }
    sha256_update(s, lenb, 8);
    for (int i = 0; i < 8; i++) {
        wr32(out + 4 * i, s->h[i]);
    }
}

// CRC-32 (STUN FINGERPRINT, reflected 0xEDB88320) and CRC-32c (SCTP).
static uint32_t crc32_ieee(const uint8_t *d, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (-(int32_t)(c & 1)));
        }
    }
    return ~c;
}

static uint32_t crc32c(const uint8_t *d, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0x82F63B78u & (-(int32_t)(c & 1)));
        }
    }
    return ~c;
}

// --- SDP --------------------------------------------------------------------

struct Offer {
    char ufrag[32];
    char pwd[96];
    char fp[128];
    char setup[16];
    uint16_t sctp_port;
    uint32_t max_msg;
};

static void sdp_field(const char *offer, const char *prefix, char *out, size_t cap)
{
    const char *p = offer;
    size_t plen = strlen(prefix);
    while (p && *p) {
        const char *eol = strstr(p, "\r\n");
        size_t line_len = eol ? (size_t)(eol - p) : strlen(p);
        if (line_len > plen && strncmp(p, prefix, plen) == 0) {
            size_t n = line_len - plen;
            if (n >= cap) {
                n = cap - 1;
            }
            memcpy(out, p + plen, n);
            out[n] = '\0';
            return;
        }
        p = eol ? eol + 2 : NULL;
    }
}

static bool parse_offer(const char *sdp, struct Offer *o)
{
    memset(o, 0, sizeof(*o));
    sdp_field(sdp, "a=ice-ufrag:", o->ufrag, sizeof(o->ufrag));
    sdp_field(sdp, "a=ice-pwd:", o->pwd, sizeof(o->pwd));
    sdp_field(sdp, "a=fingerprint:sha-256 ", o->fp, sizeof(o->fp));
    sdp_field(sdp, "a=setup:", o->setup, sizeof(o->setup));
    char tmp[16];
    sdp_field(sdp, "a=sctp-port:", tmp, sizeof(tmp));
    o->sctp_port = tmp[0] ? (uint16_t)atoi(tmp) : 5000;
    char tmp2[16];
    sdp_field(sdp, "a=max-message-size:", tmp2, sizeof(tmp2));
    o->max_msg = tmp2[0] ? (uint32_t)strtoul(tmp2, NULL, 10) : 65536;
    return o->ufrag[0] && o->pwd[0] && o->fp[0];
}

static void fmt_fingerprint(const uint8_t hash[32], char *out, size_t cap)
{
    size_t pos = 0;
    for (int i = 0; i < 32; i++) {
        int n = snprintf(out + pos, cap - pos, i ? ":%02X" : "%02X", hash[i]);
        if (n <= 0) {
            break;
        }
        pos += (size_t)n;
    }
}

// --- STUN -------------------------------------------------------------------

static bool stun_parse_attr(const uint8_t *d, size_t n, size_t *off, uint16_t *type,
                            const uint8_t **val, uint16_t *len)
{
    if (*off + 4 > n) {
        return false;
    }
    *type = rd16(d + *off);
    *len = rd16(d + *off + 2);
    if (*off + 4 + *len > n) {
        return false;
    }
    *val = d + *off + 4;
    *off += 4 + ((*len + 3u) & ~3u);
    return true;
}

static int raw_send(Peer *p, const struct sockaddr_in *dst, const uint8_t *d, size_t n)
{
    int rc = zsock_sendto(p->fd, d, n, 0, (const struct sockaddr *)dst, sizeof(*dst));
    return rc < 0 ? -errno : rc;
}

// Route a datagram over the current ICE path: direct to the peer, or wrapped
// in a TURN Send Indication when the relay was selected.
static int peer_send(Peer *p, const struct sockaddr_in *dst, const uint8_t *d,
                     size_t n);

static int turn_send_indication(Peer *p, const struct sockaddr_in *peer,
                                const uint8_t *d, size_t n);

static int turn_dispatch_data(Peer *p, const uint8_t *d, size_t n,
                              uint8_t *out, size_t cap);

static bool same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b);

static void stun_binding_response(Peer *p, const struct sockaddr_in *src,
                                  const uint8_t *req, size_t req_len)
{
    const uint8_t *user = NULL, *mi = NULL;
    uint16_t user_len = 0, mi_len = 0;
    size_t mi_off = 0, off = 20;
    uint16_t type;
    const uint8_t *val;
    uint16_t len;

    while (stun_parse_attr(req, req_len, &off, &type, &val, &len)) {
        if (type == 0x0006) {
            user = val;
            user_len = len;
        } else if (type == 0x0008) {
            mi = val;
            mi_len = len;
            mi_off = (size_t)(val - req) - 4;
        }
    }

    char uname[80];
    size_t n = user_len < sizeof(uname) - 1 ? user_len : sizeof(uname) - 1;
    memcpy(uname, user, n);
    uname[n] = '\0';


    // Verify MESSAGE-INTEGRITY over the request with the length adjusted to
    // include the integrity attribute itself.
    uint8_t tmp[512];
    if (mi_off + 24 > sizeof(tmp) || mi_off + 24 > req_len) {
        return;
    }
    memcpy(tmp, req, mi_off);
    wr16(tmp + 2, (uint16_t)(mi_off + 24 - 20));
    uint8_t mac[20];
    hmac_sha1((const uint8_t *)p->local_pwd, strlen(p->local_pwd), tmp, mi_off, mac);
    if (memcmp(mac, mi, 20) != 0) {
        return;
    }

    uint8_t resp[160];
    memset(resp, 0, sizeof(resp));
    wr16(resp, 0x0101);
    wr16(resp + 2, 0);
    wr32(resp + 4, STUN_MAGIC);
    memcpy(resp + 8, req + 8, 12);
    size_t pos = 20;

    // XOR-MAPPED-ADDRESS
    wr16(resp + pos, 0x0020);
    wr16(resp + pos + 2, 8);
    resp[pos + 4] = 0;
    resp[pos + 5] = 0x01;
    wr16(resp + pos + 6, (uint16_t)(ntohs(src->sin_port) ^ 0x2112));
    wr32(resp + pos + 8, ntohl(src->sin_addr.s_addr) ^ STUN_MAGIC);
    pos += 12;

    // MESSAGE-INTEGRITY
    wr16(resp + pos, 0x0008);
    wr16(resp + pos + 2, 20);
    wr16(resp + 2, (uint16_t)(pos + 24 - 20));
    hmac_sha1((const uint8_t *)p->local_pwd, strlen(p->local_pwd), resp, pos, mac);
    memcpy(resp + pos + 4, mac, 20);
    pos += 24;

    // FINGERPRINT
    wr16(resp + pos, 0x8028);
    wr16(resp + pos + 2, 4);
    wr16(resp + 2, (uint16_t)(pos + 8 - 20));
    uint32_t fp = crc32_ieee(resp, pos) ^ STUN_CRC_XOR;
    wr32(resp + pos + 4, fp);
    pos += 8;

    wr16(resp + 2, (uint16_t)(pos - 20));
    peer_send(p, src, resp, pos);
    p->remote_validated = true;
}

static bool stun_probe(Peer *p, const char *server, char *ip_out, size_t cap,
                      uint16_t *port_out)
{
    if (!server || !server[0] || p->fd < 0) {
        return false;
    }
    const char *host = server;
    if (strncmp(host, "stun:", 5) == 0) {
        host += 5;
    }
    char hostbuf[64];
    snprintf(hostbuf, sizeof(hostbuf), "%s", host);
    char *colon = strrchr(hostbuf, ':');
    uint16_t port = 3478;
    if (colon) {
        *colon = '\0';
        port = (uint16_t)atoi(colon + 1);
    }
    struct zsock_addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct zsock_addrinfo *res = NULL;
    if (zsock_getaddrinfo(hostbuf, NULL, &hints, &res) != 0 || !res) {
        return false;
    }
    struct sockaddr_in dst;
    memcpy(&dst, res->ai_addr, sizeof(dst));
    dst.sin_port = htons(port);
    zsock_freeaddrinfo(res);

    uint8_t req[20];
    memset(req, 0, sizeof(req));
    wr16(req, 0x0001);
    wr32(req + 4, STUN_MAGIC);
    for (int i = 0; i < 3; i++) {
        wr32(req + 8 + 4 * i, rng_next());
    }
    struct sockaddr_in saved = p->cur;
    bool saved_have = p->have_cur;
    p->cur = dst;
    p->have_cur = true;
    raw_send(p, &dst, req, sizeof(req));

    bool ok = false;
    struct zsock_pollfd pfd = {.fd = p->fd, .events = ZSOCK_POLLIN};
    if (zsock_poll(&pfd, 1, 700) > 0) {
        uint8_t resp[256];
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        int n = zsock_recvfrom(p->fd, resp, sizeof(resp), 0, (struct sockaddr *)&src, &sl);
        if (n >= 20 && (rd16(resp) == 0x0101 || rd16(resp) == 0x0001)) {
            size_t off = 20;
            uint16_t type, len;
            const uint8_t *val;
            while (stun_parse_attr(resp, (size_t)n, &off, &type, &val, &len)) {
                if (type == 0x0020 && len >= 8) {
                    uint16_t port_v = (uint16_t)(rd16(val + 2) ^ 0x2112);
                    uint32_t ip = rd32(val + 4) ^ STUN_MAGIC;
                    snprintf(ip_out, cap, "%u.%u.%u.%u", (unsigned)(ip >> 24),
                             (unsigned)((ip >> 16) & 0xff),
                             (unsigned)((ip >> 8) & 0xff), (unsigned)(ip & 0xff));
                    *port_out = port_v;
                    ok = true;
                    break;
                }
            }
        }
    }
    p->cur = saved;
    p->have_cur = saved_have;
    return ok;
}

// --- TURN -------------------------------------------------------------------
//
// A small RFC 5766 client. The device needs a relayed candidate, permission
// creation, and Send/Data Indications; channel binds are never requested, so
// browser payloads always arrive as Data Indications that we unwrap here.
//
// TURN requests are synchronous (one outstanding request, short timeout) which
// keeps the state machine tiny. They only run during allocation, when the
// browser trickles a candidate, and on the periodic refresh.

static void addr_fmt(const struct sockaddr_in *a, char *out, size_t cap)
{
    uint32_t ip = ntohl(a->sin_addr.s_addr);
    snprintf(out, cap, "%u.%u.%u.%u", (unsigned)(ip >> 24),
             (unsigned)((ip >> 16) & 0xff), (unsigned)((ip >> 8) & 0xff),
             (unsigned)(ip & 0xff));
}

static bool same_ip(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr;
}

static size_t stun_begin(uint8_t *b, size_t cap, uint16_t type)
{
    if (cap < 20) {
        return 0;
    }
    memset(b, 0, 20);
    wr16(b, type);
    wr32(b + 4, STUN_MAGIC);
    for (int i = 0; i < 3; i++) {
        wr32(b + 8 + 4 * i, rng_next());
    }
    return 20;
}

static bool stun_add(uint8_t *b, size_t cap, size_t *pos, uint16_t type,
                     const void *val, uint16_t len)
{
    size_t need = 4 + ((len + 3u) & ~3u);
    if (*pos + need > cap) {
        return false;
    }
    wr16(b + *pos, type);
    wr16(b + *pos + 2, len);
    if (len) {
        memcpy(b + *pos + 4, val, len);
    }
    if (len & 3u) {
        memset(b + *pos + 4 + len, 0, 4 - (len & 3u));
    }
    *pos += need;
    return true;
}

static bool stun_add_xor_addr(uint8_t *b, size_t cap, size_t *pos, uint16_t type,
                              const struct sockaddr_in *a)
{
    uint8_t v[8];
    v[0] = 0;
    v[1] = 0x01;
    wr16(v + 2, (uint16_t)(ntohs(a->sin_port) ^ 0x2112));
    wr32(v + 4, ntohl(a->sin_addr.s_addr) ^ STUN_MAGIC);
    return stun_add(b, cap, pos, type, v, sizeof(v));
}

static bool stun_get_xor_addr(const uint8_t *val, uint16_t len,
                              struct sockaddr_in *a)
{
    if (len < 8 || val[1] != 0x01) {
        return false;
    }
    memset(a, 0, sizeof(*a));
    a->sin_family = AF_INET;
    a->sin_port = htons((uint16_t)(rd16(val + 2) ^ 0x2112));
    a->sin_addr.s_addr = htonl(rd32(val + 4) ^ STUN_MAGIC);
    return true;
}

static void stun_end(uint8_t *b, size_t pos)
{
    wr16(b + 2, (uint16_t)(pos - 20));
}

static bool stun_integrity(Peer *p, uint8_t *b, size_t cap, size_t *pos)
{
    if (*pos + 24 > cap) {
        return false;
    }
    // The HMAC covers the message up to the MESSAGE-INTEGRITY header, with the
    // length field already accounting for the 24-byte attribute itself.
    wr16(b + 2, (uint16_t)(*pos + 24 - 20));
    uint8_t mac[20];
    hmac_sha1(p->turn_key, sizeof(p->turn_key), b, *pos, mac);
    wr16(b + *pos, STUN_ATTR_MESSAGE_INTEGRITY);
    wr16(b + *pos + 2, 20);
    memcpy(b + *pos + 4, mac, 20);
    *pos += 24;
    return true;
}

struct TurnReply {
    uint16_t type;
    int code;
    const uint8_t *realm;
    uint16_t realm_len;
    const uint8_t *nonce;
    uint16_t nonce_len;
    bool have_relay;
    struct sockaddr_in relay;
    uint32_t lifetime;
};

static bool turn_parse(const uint8_t *d, size_t n, struct TurnReply *r)
{
    memset(r, 0, sizeof(*r));
    if (n < 20 || rd32(d + 4) != STUN_MAGIC) {
        return false;
    }
    r->type = rd16(d);
    size_t off = 20;
    uint16_t type, len;
    const uint8_t *val;
    while (stun_parse_attr(d, n, &off, &type, &val, &len)) {
        switch (type) {
        case STUN_ATTR_ERROR_CODE:
            if (len >= 4) {
                r->code = (val[2] & 0x07) * 100 + val[3];
            }
            break;
        case STUN_ATTR_REALM:
            r->realm = val;
            r->realm_len = len;
            break;
        case STUN_ATTR_NONCE:
            r->nonce = val;
            r->nonce_len = len;
            break;
        case STUN_ATTR_XOR_RELAYED:
            if (stun_get_xor_addr(val, len, &r->relay)) {
                r->have_relay = true;
            }
            break;
        case STUN_ATTR_LIFETIME:
            if (len >= 4) {
                r->lifetime = rd32(val);
            }
            break;
        default:
            break;
        }
    }
    return true;
}

static void turn_note_auth(Peer *p, const struct TurnReply *r)
{
    bool changed = false;
    if (r->realm && r->realm_len) {
        size_t n = r->realm_len < sizeof(p->turn_realm) - 1
                       ? r->realm_len
                       : sizeof(p->turn_realm) - 1;
        if (n != strlen(p->turn_realm) || memcmp(p->turn_realm, r->realm, n) != 0) {
            memcpy(p->turn_realm, r->realm, n);
            p->turn_realm[n] = '\0';
            changed = true;
        }
    }
    if (r->nonce && r->nonce_len) {
        size_t n = r->nonce_len < sizeof(p->turn_nonce)
                       ? r->nonce_len
                       : sizeof(p->turn_nonce);
        if (n != p->turn_nonce_len || memcmp(p->turn_nonce, r->nonce, n) != 0) {
            memcpy(p->turn_nonce, r->nonce, n);
            p->turn_nonce_len = (uint16_t)n;
            changed = true;
        }
    }
    if (changed && p->turn_realm[0]) {
        char buf[224];
        int n = snprintf(buf, sizeof(buf), "%s:%s:%s", p->turn_user,
                         p->turn_realm, p->turn_pass);
        if (n > 0 && (size_t)n < sizeof(buf)) {
            md5_digest(buf, (size_t)n, p->turn_key);
        }
    }
}

static bool turn_build(Peer *p, uint8_t *b, size_t cap, uint16_t type, bool auth,
                       const struct sockaddr_in *peer, uint32_t lifetime,
                       size_t *len)
{
    size_t pos = stun_begin(b, cap, type);
    if (!pos) {
        return false;
    }
    if (type == TURN_ALLOCATE_REQ) {
        uint8_t rt[4] = {TURN_TRANSPORT_UDP, 0, 0, 0};
        if (!stun_add(b, cap, &pos, STUN_ATTR_REQUESTED_TRANSPORT, rt,
                      sizeof(rt))) {
            return false;
        }
    } else if (type == TURN_REFRESH_REQ) {
        uint8_t lt[4];
        wr32(lt, lifetime);
        if (!stun_add(b, cap, &pos, STUN_ATTR_LIFETIME, lt, sizeof(lt))) {
            return false;
        }
    }
    if (peer && !stun_add_xor_addr(b, cap, &pos, STUN_ATTR_XOR_PEER, peer)) {
        return false;
    }
    if (auth) {
        if (!stun_add(b, cap, &pos, STUN_ATTR_USERNAME, p->turn_user,
                      (uint16_t)strlen(p->turn_user)) ||
            !stun_add(b, cap, &pos, STUN_ATTR_REALM, p->turn_realm,
                      (uint16_t)strlen(p->turn_realm)) ||
            !stun_add(b, cap, &pos, STUN_ATTR_NONCE, p->turn_nonce,
                      p->turn_nonce_len) ||
            !stun_integrity(p, b, cap, &pos)) {
            return false;
        }
    }
    stun_end(b, pos);
    *len = pos;
    return true;
}

static int turn_dispatch_data(Peer *p, const uint8_t *d, size_t n, uint8_t *out,
                              size_t cap)
{
    struct sockaddr_in peer;
    const uint8_t *data = NULL;
    uint16_t dlen = 0;
    bool have_peer = false;
    size_t off = 20;
    uint16_t type, len;
    const uint8_t *val;

    while (stun_parse_attr(d, n, &off, &type, &val, &len)) {
        if (type == STUN_ATTR_XOR_PEER) {
            have_peer = stun_get_xor_addr(val, len, &peer);
        } else if (type == STUN_ATTR_DATA) {
            data = val;
            dlen = len;
        }
    }
    if (!have_peer || !data || !p->turn_allocated) {
        return -1;
    }

    p->relay_path = true;
    p->relay_used = true;
    p->cur = peer;
    p->have_cur = true;
    // The peer may already have been recorded by a STUN connectivity check
    // from the same 5-tuple, so "unchanged" is not a reason to skip this: the
    // cookie callbacks hash the transport id, and it must be set before the
    // first ClientHello is processed.
    if (p->tls_setup && !p->tls_ready) {
        (void)mbedtls_ssl_set_client_transport_id(
            &p->ssl, (const unsigned char *)&peer, sizeof(peer));
    }

    if (dlen >= 20 && data[0] <= 0x01 && rd32(data + 4) == STUN_MAGIC) {
        if (rd16(data) == STUN_BINDING_REQ) {
            stun_binding_response(p, &peer, data, dlen);
        }
        return -1;
    }
    if (!out || cap == 0) {
        return -1;
    }
    size_t copy = dlen < cap ? dlen : cap;
    memcpy(out, data, copy);
    return (int)copy;
}

static int turn_send_indication(Peer *p, const struct sockaddr_in *peer,
                                const uint8_t *d, size_t n)
{
    if (n > 1400) {
        return -EMSGSIZE;
    }
    uint8_t head[32];
    memset(head, 0, sizeof(head));
    size_t padded = (n + 3u) & ~3u;
    wr16(head, TURN_SEND_IND);
    // Message length: XOR-PEER-ADDRESS (4 + 8) plus DATA (4 + padded payload).
    wr16(head + 2, (uint16_t)(16 + padded));
    wr32(head + 4, STUN_MAGIC);
    for (int i = 0; i < 3; i++) {
        wr32(head + 8 + 4 * i, rng_next());
    }
    wr16(head + 20, STUN_ATTR_XOR_PEER);
    wr16(head + 22, 8);
    head[24] = 0;
    head[25] = 1;
    wr16(head + 26, (uint16_t)(ntohs(peer->sin_port) ^ 0x2112));
    wr32(head + 28, ntohl(peer->sin_addr.s_addr) ^ STUN_MAGIC);

    uint8_t datahdr[4];
    wr16(datahdr, STUN_ATTR_DATA);
    wr16(datahdr + 2, (uint16_t)n);
    static const uint8_t zero[4] = {0, 0, 0, 0};

    struct net_iovec iov[4] = {
        {head, sizeof(head)},
        {datahdr, sizeof(datahdr)},
        {(void *)d, n},
        {(void *)zero, padded - n},
    };
    struct net_msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_name = (void *)&p->turn_srv;
    msg.msg_namelen = sizeof(p->turn_srv);
    msg.msg_iov = iov;
    msg.msg_iovlen = 4;
    int rc = zsock_sendmsg(p->fd, &msg, 0);
    return rc < 0 ? -errno : rc;
}

static int peer_send(Peer *p, const struct sockaddr_in *dst, const uint8_t *d,
                     size_t n)
{
    if (p->relay_path && p->turn_allocated) {
        return turn_send_indication(p, dst, d, n);
    }
    return raw_send(p, dst, d, n);
}

static int turn_send_wait(Peer *p, uint8_t *req, size_t req_len, uint8_t *resp,
                          size_t cap, size_t *resp_len)
{
    uint8_t txid[12];
    memcpy(txid, req + 8, sizeof(txid));
    if (raw_send(p, &p->turn_srv, req, req_len) < 0) {
        return -1;
    }
    int64_t end = k_uptime_get() + TURN_TIMEOUT_MS;
    for (;;) {
        int64_t left = end - k_uptime_get();
        if (left <= 0) {
            return -1;
        }
        struct zsock_pollfd pfd = {.fd = p->fd, .events = ZSOCK_POLLIN};
        if (zsock_poll(&pfd, 1, (int)left) <= 0) {
            return -1;
        }
        uint8_t raw[512];
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        int n = zsock_recvfrom(p->fd, raw, sizeof(raw), 0,
                               (struct sockaddr *)&src, &sl);
        if (n < 20 || raw[0] > 0x01 || rd32(raw + 4) != STUN_MAGIC) {
            continue;
        }
        uint16_t mt = rd16(raw);
        if (mt == STUN_BINDING_REQ) {
            // A direct browser check raced the TURN response.
            p->relay_path = false;
            p->cur = src;
            p->have_cur = true;
            stun_binding_response(p, &src, raw, (size_t)n);
            continue;
        }
        if (mt == TURN_DATA_IND) {
            (void)turn_dispatch_data(p, raw, (size_t)n, NULL, 0);
            continue;
        }
        if (memcmp(raw + 8, txid, sizeof(txid)) != 0) {
            continue;
        }
        size_t copy = (size_t)n < cap ? (size_t)n : cap;
        memcpy(resp, raw, copy);
        *resp_len = copy;
        return mt;
    }
}

// Delete an allocation on this 5-tuple. Used on teardown, and to recover from
// a 437 Allocation Mismatch left behind when the peer is restarted faster than
// the server's allocation lifetime.
static bool turn_deallocate(Peer *p)
{
    uint8_t req[256], resp[320];
    size_t len = 0, rlen = 0;

    if (!p->turn_realm[0] || !p->turn_nonce_len) {
        return false;
    }
    if (!turn_build(p, req, sizeof(req), TURN_REFRESH_REQ, true, NULL, 0,
                    &len)) {
        return false;
    }
    int mt = turn_send_wait(p, req, len, resp, sizeof(resp), &rlen);
    if (mt == TURN_REFRESH_OK) {
        p->turn_allocated = false;
        p->have_relay = false;
        p->relay_path = false;
        printk("webrtc: TURN allocation released\n");
        return true;
    }
    if (mt == TURN_ERROR) {
        struct TurnReply r;
        if (turn_parse(resp, rlen, &r)) {
            turn_note_auth(p, &r);
        }
    }
    return false;
}

// coturn answers 437 instead of a 401 challenge when our 5-tuple still owns an
// allocation (fast peer restart or device reboot), so we never learn the realm
// needed to authenticate the Refresh(0) that clears it. Ask from a throwaway
// socket: it has no allocation, so the server answers with a normal challenge.
static bool turn_bootstrap_auth(Peer *p)
{
    int tmp = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (tmp < 0) {
        return false;
    }
    zsock_fcntl(tmp, F_SETFL, O_NONBLOCK);

    uint8_t req[320], resp[320];
    size_t len = 0, rlen = 0;
    bool ok = false;
    int saved = p->fd;
    p->fd = tmp;
    if (turn_build(p, req, sizeof(req), TURN_ALLOCATE_REQ, false, NULL,
                   TURN_LIFETIME, &len)) {
        int mt = turn_send_wait(p, req, len, resp, sizeof(resp), &rlen);
        if (mt == TURN_ERROR) {
            struct TurnReply r;
            if (turn_parse(resp, rlen, &r) &&
                (r.code == 401 || r.code == 438)) {
                turn_note_auth(p, &r);
                ok = p->turn_realm[0] && p->turn_nonce_len > 0;
            }
        }
    }
    p->fd = saved;
    zsock_close(tmp);
    return ok;
}

static bool turn_allocate(Peer *p)
{
    uint8_t req[320], resp[320];
    size_t len = 0, rlen = 0;
    int mt = 0;
    int auth_tries = 0;
    bool released = false;

    for (int attempt = 0; attempt < 12; attempt++) {
        bool auth = p->turn_realm[0] && p->turn_nonce_len > 0;
        if (!turn_build(p, req, sizeof(req), TURN_ALLOCATE_REQ, auth, NULL,
                        TURN_LIFETIME, &len)) {
            snprintf(p->turn_error, sizeof(p->turn_error), "alloc build");
            return false;
        }
        mt = turn_send_wait(p, req, len, resp, sizeof(resp), &rlen);
        if (mt < 0) {
            snprintf(p->turn_error, sizeof(p->turn_error), "no response");
            return false;
        }
        struct TurnReply r;
        if (!turn_parse(resp, rlen, &r)) {
            snprintf(p->turn_error, sizeof(p->turn_error), "bad response");
            return false;
        }
        if (mt == TURN_ALLOCATE_OK && r.have_relay) {
            p->relay_addr = r.relay;
            p->have_relay = true;
            p->turn_allocated = true;
            addr_fmt(&r.relay, p->relay_ip, sizeof(p->relay_ip));
            p->relay_port = ntohs(r.relay.sin_port);
            uint32_t life = r.lifetime ? r.lifetime : TURN_LIFETIME;
            p->turn_refresh_at = k_uptime_get() + (int64_t)life * 800;
            p->turn_perm_at = k_uptime_get() + 180000;
            p->turn_error[0] = '\0';
            return true;
        }
        if (mt == TURN_ERROR) {
            turn_note_auth(p, &r);
            if (r.code == 401 || r.code == 438) {
                if (++auth_tries <= 3) {
                    continue;
                }
                snprintf(p->turn_error, sizeof(p->turn_error), "auth %d",
                         r.code);
                return false;
            }
            if (r.code == 437) {
                // coturn answers 437 before authentication when the 5-tuple
                // still has an allocation, and keeps the session in a closing
                // state for a moment after Refresh(0). Delete it if we know
                // the realm, then poll until the mismatch clears.
                if (!released) {
                    released = true;
                    if (!p->turn_realm[0] || !p->turn_nonce_len) {
                        (void)turn_bootstrap_auth(p);
                    }
                    if (p->turn_realm[0]) {
                        bool gone = turn_deallocate(p);
                        printk("webrtc: TURN 437, deallocate %s\n",
                               gone ? "ok" : "failed");
                    }
                }
                k_sleep(K_MSEC(250));
                continue;
            }
            snprintf(p->turn_error, sizeof(p->turn_error), "alloc %d", r.code);
            return false;
        }
        snprintf(p->turn_error, sizeof(p->turn_error), "alloc type %04x", mt);
        return false;
    }
    if (!p->turn_error[0]) {
        snprintf(p->turn_error, sizeof(p->turn_error), "alloc retry");
    }
    return false;
}

static void turn_remember(Peer *p, const struct sockaddr_in *peer)
{
    k_mutex_lock(&p->turn_lock, K_FOREVER);
    for (uint8_t i = 0; i < p->turn_perm_count; i++) {
        if (same_ip(&p->turn_perms[i], peer)) {
            p->turn_perms[i] = *peer;
            k_mutex_unlock(&p->turn_lock);
            return;
        }
    }
    if (p->turn_perm_count < TURN_CAND_MAX) {
        p->turn_perms[p->turn_perm_count++] = *peer;
    }
    k_mutex_unlock(&p->turn_lock);
}

static bool turn_set_permission(Peer *p, const struct sockaddr_in *peer)
{
    uint8_t req[320], resp[320];
    size_t len = 0, rlen = 0;

    if (!p->turn_allocated) {
        return false;
    }
    for (int attempt = 0; attempt < 2; attempt++) {
        bool auth = p->turn_realm[0] && p->turn_nonce_len > 0;
        if (!turn_build(p, req, sizeof(req), TURN_PERM_REQ, auth, peer,
                        TURN_LIFETIME, &len)) {
            return false;
        }
        int mt = turn_send_wait(p, req, len, resp, sizeof(resp), &rlen);
        if (mt < 0) {
            return false;
        }
        struct TurnReply r;
        if (!turn_parse(resp, rlen, &r)) {
            return false;
        }
        if (mt == TURN_PERM_OK) {
            turn_remember(p, peer);
            return true;
        }
        if (mt == TURN_ERROR && (r.code == 401 || r.code == 438) &&
            attempt == 0) {
            turn_note_auth(p, &r);
            continue;
        }
        if (mt == TURN_ERROR) {
            snprintf(p->turn_error, sizeof(p->turn_error), "perm %d", r.code);
        }
        return false;
    }
    return false;
}

static void turn_refresh(Peer *p)
{
    uint8_t req[320], resp[320];
    size_t len = 0, rlen = 0;

    for (int attempt = 0; attempt < 2; attempt++) {
        bool auth = p->turn_realm[0] && p->turn_nonce_len > 0;
        if (!turn_build(p, req, sizeof(req), TURN_REFRESH_REQ, auth, NULL,
                        TURN_LIFETIME, &len)) {
            return;
        }
        int mt = turn_send_wait(p, req, len, resp, sizeof(resp), &rlen);
        if (mt < 0) {
            return;
        }
        struct TurnReply r;
        if (!turn_parse(resp, rlen, &r)) {
            return;
        }
        if (mt == TURN_REFRESH_OK) {
            uint32_t life = r.lifetime ? r.lifetime : TURN_LIFETIME;
            p->turn_refresh_at = k_uptime_get() + (int64_t)life * 800;
            return;
        }
        if (mt == TURN_ERROR && (r.code == 401 || r.code == 438) &&
            attempt == 0) {
            turn_note_auth(p, &r);
            continue;
        }
        if (mt == TURN_ERROR) {
            snprintf(p->turn_error, sizeof(p->turn_error), "refresh %d", r.code);
        }
        return;
    }
}

static void turn_queue(Peer *p, const struct sockaddr_in *peer)
{
    if (!p->turn_allocated || peer->sin_addr.s_addr == 0 || peer->sin_port == 0) {
        return;
    }
    k_mutex_lock(&p->turn_lock, K_FOREVER);
    for (uint8_t i = 0; i < p->turn_perm_count; i++) {
        if (same_ip(&p->turn_perms[i], peer)) {
            k_mutex_unlock(&p->turn_lock);
            return;
        }
    }
    for (uint8_t i = 0; i < p->turn_cand_count; i++) {
        uint8_t idx = (uint8_t)((p->turn_cand_head + i) % TURN_CAND_MAX);
        if (same_ip(&p->turn_cands[idx], peer)) {
            k_mutex_unlock(&p->turn_lock);
            return;
        }
    }
    if (p->turn_cand_count < TURN_CAND_MAX) {
        uint8_t idx =
            (uint8_t)((p->turn_cand_head + p->turn_cand_count) % TURN_CAND_MAX);
        p->turn_cands[idx] = *peer;
        p->turn_cand_count++;
    }
    k_mutex_unlock(&p->turn_lock);
}

static bool candidate_addr(const char *cand, struct sockaddr_in *out)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", cand);
    char *toks[20];
    int nt = 0;
    char *save = NULL;
    for (char *t = strtok_r(buf, " \t\r\n", &save); t && nt < 20;
         t = strtok_r(NULL, " \t\r\n", &save)) {
        toks[nt++] = t;
    }
    int typ = -1, udp = -1;
    for (int i = 0; i < nt; i++) {
        if (strcmp(toks[i], "typ") == 0) {
            typ = i;
        } else if (strcmp(toks[i], "udp") == 0) {
            udp = i;
        }
    }
    if (typ < 2 || udp < 0 || udp > typ) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    if (zsock_inet_pton(AF_INET, toks[typ - 2], &out->sin_addr) != 1) {
        return false;
    }
    long port = strtol(toks[typ - 1], NULL, 10);
    if (port <= 0 || port > 65535) {
        return false;
    }
    out->sin_port = htons((uint16_t)port);
    return out->sin_addr.s_addr != 0;
}

static void offer_candidates(Peer *p, const char *sdp)
{
    const char *line = sdp;
    while (line && *line) {
        const char *eol = strstr(line, "\r\n");
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        if (len > 12 && strncmp(line, "a=candidate:", 12) == 0) {
            char tmp[256];
            size_t n = len < sizeof(tmp) - 1 ? len : sizeof(tmp) - 1;
            memcpy(tmp, line, n);
            tmp[n] = '\0';
            struct sockaddr_in a;
            if (candidate_addr(tmp, &a)) {
                turn_queue(p, &a);
            }
        }
        line = eol ? eol + 2 : NULL;
    }
}

static void turn_service(Peer *p)
{
    if (!p->turn_allocated) {
        return;
    }
    int64_t now = k_uptime_get();
    if (now >= p->turn_refresh_at) {
        turn_refresh(p);
        if (p->turn_refresh_at <= now) {
            p->turn_refresh_at = now + 240000;
        }
    }

    for (;;) {
        struct sockaddr_in cand;
        bool got = false;
        k_mutex_lock(&p->turn_lock, K_FOREVER);
        if (p->turn_cand_count) {
            cand = p->turn_cands[p->turn_cand_head];
            p->turn_cand_head =
                (uint8_t)((p->turn_cand_head + 1) % TURN_CAND_MAX);
            p->turn_cand_count--;
            got = true;
        }
        k_mutex_unlock(&p->turn_lock);
        if (!got) {
            break;
        }
        (void)turn_set_permission(p, &cand);
    }

    if (now >= p->turn_perm_at) {
        struct sockaddr_in perms[TURN_CAND_MAX];
        uint8_t count;
        k_mutex_lock(&p->turn_lock, K_FOREVER);
        count = p->turn_perm_count;
        memcpy(perms, p->turn_perms, sizeof(perms));
        k_mutex_unlock(&p->turn_lock);
        for (uint8_t i = 0; i < count; i++) {
            (void)turn_set_permission(p, &perms[i]);
        }
        p->turn_perm_at = now + 180000;
    }
}

static bool turn_setup(Peer *p)
{
    p->turn_error[0] = '\0';
    if (!p->turn_url[0]) {
        return false;
    }
    const char *host = p->turn_url;
    if (strncmp(host, "turn:", 5) == 0) {
        host += 5;
    } else {
        snprintf(p->turn_error, sizeof(p->turn_error), "unsupported url");
        return false;
    }
    char hostbuf[64];
    snprintf(hostbuf, sizeof(hostbuf), "%s", host);
    char *query = strchr(hostbuf, '?');
    if (query) {
        *query = '\0';
    }
    uint16_t port = 3478;
    char *colon = strrchr(hostbuf, ':');
    if (colon) {
        *colon = '\0';
        port = (uint16_t)atoi(colon + 1);
    }

    struct zsock_addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct zsock_addrinfo *res = NULL;
    if (zsock_getaddrinfo(hostbuf, NULL, &hints, &res) != 0 || !res) {
        snprintf(p->turn_error, sizeof(p->turn_error), "resolve failed");
        return false;
    }
    memcpy(&p->turn_srv, res->ai_addr, sizeof(p->turn_srv));
    p->turn_srv.sin_port = htons(port);
    zsock_freeaddrinfo(res);

    if (!turn_allocate(p)) {
        printk("webrtc: TURN unavailable (%s)\n", p->turn_error);
        return false;
    }
    printk("webrtc: TURN relay candidate %s:%u\n", p->relay_ip, p->relay_port);
    return true;
}

// --- DTLS glue --------------------------------------------------------------

static bool same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static int dtls_send_cb(void *ctx, const unsigned char *buf, size_t len)
{
    Peer *p = (Peer *)ctx;
    if (!p->have_cur) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    int rc = peer_send(p, &p->cur, buf, len);
    if (rc < 0) {
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    // mbedtls compares the callback return with the DTLS record length it asked
    // to send. A relayed datagram is wrapped in a TURN Send Indication, so raw
    // sendmsg() returns extra framing bytes; reporting those makes mbedtls see
    // "sent more than out_left" and fail the handshake with -0x6c00.
    return (int)len;
}

static int dtls_recv_cb(void *ctx, unsigned char *buf, size_t len)
{
    Peer *p = (Peer *)ctx;
    uint8_t raw[1536];

    for (;;) {
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        int n = zsock_recvfrom(p->fd, raw, sizeof(raw), 0, (struct sockaddr *)&src, &sl);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return MBEDTLS_ERR_SSL_WANT_READ;
            }
            return MBEDTLS_ERR_NET_RECV_FAILED;
        }
        if (n < 20) {
            continue;
        }
        if (raw[0] <= 0x01 && rd32(raw + 4) == STUN_MAGIC) {
            uint16_t mt = rd16(raw);
            if (mt == TURN_DATA_IND) {
                int got = turn_dispatch_data(p, raw, (size_t)n, buf, len);
                if (got > 0) {
                    return got;
                }
                continue;
            }
            if (mt == STUN_BINDING_REQ) {
                p->relay_path = false;
                p->cur = src;
                p->have_cur = true;
                stun_binding_response(p, &src, raw, (size_t)n);
            }
            continue;
        }
        p->relay_path = false;
        p->cur = src;
        p->have_cur = true;
        // Always refresh the cookie transport id while handshaking. STUN
        // checks share this socket and may have populated p->cur first, which
        // would otherwise leave the DTLS cookie bound to a stale/empty id.
        if (!p->tls_ready) {
            (void)mbedtls_ssl_set_client_transport_id(&p->ssl,
                                                      (const unsigned char *)&src,
                                                      sizeof(src));
        }
        if ((size_t)n > len) {
            n = (int)len;
        }
        memcpy(buf, raw, (size_t)n);
        return n;
    }
}

static void dtls_timer_set(void *ctx, uint32_t int_ms, uint32_t fin_ms)
{
    Peer *p = (Peer *)ctx;
    p->timer_int_ms = int_ms;
    p->timer_fin_ms = fin_ms;
    p->timer_int_start = k_uptime_get();
    p->timer_fin_start = p->timer_int_start;
}

static int dtls_timer_get(void *ctx)
{
    Peer *p = (Peer *)ctx;
    if (p->timer_fin_ms == 0) {
        return -1;
    }
    int64_t now = k_uptime_get();
    if (now >= p->timer_fin_start + p->timer_fin_ms) {
        return 2;
    }
    if (now >= p->timer_int_start + p->timer_int_ms) {
        return 1;
    }
    return 0;
}

static int dtls_cookie_write(void *ctx, unsigned char **p, unsigned char *end,
                             const unsigned char *info, size_t ilen)
{
    ARG_UNUSED(ctx);
    if ((size_t)(end - *p) < 16) {
        return -1;
    }
    uint32_t t = k_uptime_get_32();
    uint32_t tag = fnv32(info, ilen, 2166136261u ^ 0x4d524146u);
    tag = fnv32((const uint8_t *)&t, sizeof(t), tag);
    wr32(*p, t);
    wr32(*p + 4, tag);
    for (int i = 0; i < 8; i++) {
        (*p)[8 + i] = (unsigned char)(rng_next() >> 24);
    }
    *p += 16;
    return 0;
}

static int dtls_cookie_check(void *ctx, const unsigned char *cookie, size_t clen,
                             const unsigned char *info, size_t ilen)
{
    ARG_UNUSED(ctx);
    if (clen != 16) {
        return -1;
    }
    uint32_t t = rd32(cookie);
    uint32_t tag = fnv32(info, ilen, 2166136261u ^ 0x4d524146u);
    tag = fnv32((const uint8_t *)&t, sizeof(t), tag);
    if (tag != rd32(cookie + 4)) {
        printk("webrtc: DTLS cookie mismatch idlen=%u want=%08x got=%08x\n",
               (unsigned)ilen, (unsigned)rd32(cookie + 4), (unsigned)tag);
        return -1;
    }
    if ((uint32_t)(k_uptime_get_32() - t) > 60000u) {
        return -1;
    }
    return 0;
}

// --- SCTP -------------------------------------------------------------------

static int sctp_packet_send(Peer *p, const uint8_t *d, size_t n)
{
    int rc = mbedtls_ssl_write(&p->ssl, d, n);
    if (rc < 0) {
        printk("webrtc: SCTP send failed -0x%04x (len %u)\n", -rc, (unsigned)n);
        return -1;
    }
    return rc;
}

static int sctp_packet_build(Peer *p, uint8_t *pkt, uint16_t src_port,
                             uint16_t dst_port, uint32_t vtag, uint8_t *body,
                             size_t body_len)
{
    wr16(pkt, src_port);
    wr16(pkt + 2, dst_port);
    wr32(pkt + 4, vtag);
    wr32(pkt + 8, 0);
    memcpy(pkt + 12, body, body_len);
    size_t padded = (body_len + 3u) & ~(size_t)3u;
    if (padded > body_len) {
        memset(pkt + 12 + body_len, 0, padded - body_len);
    }
    uint32_t crc = crc32c(pkt, padded + 12);
    wr32_le(pkt + 8, crc);
    return (int)(padded + 12);
}

static int send_data(Peer *p, uint16_t stream, uint16_t ssn, uint8_t ppid,
                     uint32_t tsn, const uint8_t *data, size_t len, uint8_t flags)
{
    uint8_t body[16 + SCTP_DATA_MAX + 4];
    size_t pos = 0;
    body[pos++] = SCTP_DATA;
    body[pos++] = flags;
    uint16_t chunk_len = (uint16_t)(16 + len);
    wr16(body + pos, chunk_len);
    pos += 2;
    wr32(body + pos, tsn);
    pos += 4;
    wr16(body + pos, stream);
    pos += 2;
    wr16(body + pos, ssn);
    pos += 2;
    wr32(body + pos, ppid);
    pos += 4;
    memcpy(body + pos, data, len);
    pos += len;
    uint8_t pkt[SCTP_PKT_MAX];
    int n = sctp_packet_build(p, pkt, PEER_PORT, p->remote_port, p->remote_tag,
                              body, pos);
    return sctp_packet_send(p, pkt, (size_t)n);
}

static void sack_schedule(Peer *p)
{
    p->sack_pending = true;
    p->sack_at = k_uptime_get() + 15;
}

static void send_sack(Peer *p)
{
    uint8_t body[16 + 4 * (GAP_MAX + 2)];
    memset(body, 0, sizeof(body));
    size_t pos = 0;
    body[pos++] = SCTP_SACK;
    body[pos++] = 0;
    wr16(body + pos, (uint16_t)(16 + 4 * p->gap_count));
    pos += 2;
    wr32(body + pos, p->peer_cum_tsn);
    pos += 4;
    wr32(body + pos, 131070);
    pos += 4;
    wr16(body + pos, p->gap_count);
    wr16(body + pos + 2, 0);
    pos += 4;
    for (uint8_t i = 0; i < p->gap_count; i++) {
        uint32_t off = p->gaps[i] - p->peer_cum_tsn;
        wr16(body + pos, (uint16_t)off);
        wr16(body + pos + 2, (uint16_t)off);
        pos += 4;
    }
    uint8_t pkt[SCTP_PKT_MAX];
    int n = sctp_packet_build(p, pkt, PEER_PORT, p->remote_port, p->remote_tag,
                              body, pos);
    sctp_packet_send(p, pkt, (size_t)n);
    p->sack_pending = false;
}

static void gap_add(Peer *p, uint32_t tsn)
{
    for (uint8_t i = 0; i < p->gap_count; i++) {
        if (p->gaps[i] == tsn) {
            return;
        }
    }
    if (p->gap_count < GAP_MAX) {
        p->gaps[p->gap_count++] = tsn;
    }
}

static void rx_mark_tsn(Peer *p, uint32_t tsn)
{
    if (tsn == p->peer_cum_tsn + 1) {
        p->peer_cum_tsn = tsn;
        for (;;) {
            bool advanced = false;
            for (uint8_t i = 0; i < p->gap_count; i++) {
                if (p->gaps[i] == p->peer_cum_tsn + 1) {
                    p->peer_cum_tsn++;
                    p->gaps[i] = p->gaps[p->gap_count - 1];
                    p->gap_count--;
                    advanced = true;
                    break;
                }
            }
            if (!advanced) {
                break;
            }
        }
    } else if (tsn > p->peer_cum_tsn) {
        gap_add(p, tsn);
    }
}

static void deliver(Peer *p)
{
    if (!p->rx_cb) {
        return;
    }
    if (p->rx_len >= SCTP_RX_MAX) {
        p->rx_len = SCTP_RX_MAX - 1;
    }
    p->rx[p->rx_len] = '\0';
    p->rx_cb((const char *)p->rx);
}

static void app_fragment(Peer *p, uint8_t flags, uint32_t tsn, uint16_t stream,
                         uint16_t ssn, uint8_t ppid, const uint8_t *data, size_t len)
{
    if (ppid == DCEP_PPID) {
        if (len && data[0] == 0x03) {
            p->dcep_stream = stream;
            p->dcep_open = true;
            uint8_t ack = 0x02;
            send_data(p, stream, p->out_ssn[stream % STREAM_MAX]++, DCEP_PPID,
                      p->local_tsn++, &ack, 1, F_BIT | E_BIT);
            p->dcep_acked = true;
            printk("webrtc: data channel open on stream %u\n", stream);
        }
        return;
    }
    if (ppid != STRING_PPID && ppid != BINARY_PPID) {
        printk("webrtc: DATA ppid %u ignored\n", ppid);
        return;
    }
    if (flags & F_BIT) {
        p->rx_active = true;
        p->rx_base = tsn;
        p->rx_seen = 0;
        p->rx_expected = 0;
        p->rx_len = 0;
        p->rx_stream = stream;
        p->rx_ssn = ssn;
        p->rx_ppid = ppid;
    }
    if (!p->rx_active) {
        p->rx_active = true;
        p->rx_base = tsn;
        p->rx_seen = 0;
        p->rx_expected = 0;
        p->rx_len = 0;
    }
    uint32_t idx = tsn - p->rx_base;
    if (idx >= RX_FRAG_MAX) {
        p->rx_active = false;
        return;
    }
    size_t off = idx * SCTP_DATA_MAX;
    if (off + len > SCTP_RX_MAX) {
        p->rx_active = false;
        return;
    }
    memcpy(p->rx + off, data, len);
    if (off + len > p->rx_len) {
        p->rx_len = off + len;
    }
    p->rx_seen |= 1u << idx;
    if (flags & E_BIT) {
        p->rx_expected = (uint8_t)(idx + 1);
    }
    if (p->rx_expected && p->rx_seen == ((1u << p->rx_expected) - 1)) {
        p->rx_active = false;
        p->rx_seen = 0;
        p->rx_expected = 0;
        deliver(p);
    }
}

static void handle_init(Peer *p, const uint8_t *body, size_t len)
{
    if (len < 16) {
        return;
    }
    p->remote_tag = rd32(body);
    uint32_t init_tsn = rd32(body + 12);
    p->peer_cum_tsn = init_tsn - 1;
    p->remote_port = p->remote_port ? p->remote_port : PEER_PORT;

    uint8_t out[128];
    size_t pos = 0;
    out[pos++] = SCTP_INIT_ACK;
    out[pos++] = 0;
    wr16(out + pos, 0);
    pos += 2;
    wr32(out + pos, p->local_tag);
    pos += 4;
    wr32(out + pos, 65535 + 65535);
    pos += 4;
    wr16(out + pos, 65535);
    wr16(out + pos + 2, 65535);
    pos += 4;
    wr32(out + pos, p->local_tsn);
    pos += 4;
    // State cookie parameter: type 7, length 20 (4 header + 16 cookie).
    wr16(out + pos, 0x0007);
    wr16(out + pos + 2, 20);
    pos += 4;
    unsigned char *cp = out + pos;
    unsigned char *end = out + pos + 16;
    if (dtls_cookie_write(p, &cp, end, (const unsigned char *)&p->cur, sizeof(p->cur)) != 0) {
        return;
    }
    pos += 16;
    wr16(out + 2, (uint16_t)pos);

    uint8_t pkt[SCTP_PKT_MAX];
    int n = sctp_packet_build(p, pkt, PEER_PORT, p->remote_port, p->remote_tag,
                              out, pos);
    sctp_packet_send(p, pkt, (size_t)n);
}

static void handle_cookie_echo(Peer *p, const uint8_t *body, size_t len)
{
    if (dtls_cookie_check(p, body, len, (const unsigned char *)&p->cur, sizeof(p->cur)) != 0) {
        printk("webrtc: bad SCTP state cookie\n");
        return;
    }
    uint8_t ack[4] = {SCTP_COOKIE_ACK, 0, 0, 4};
    uint8_t pkt[SCTP_PKT_MAX];
    int n = sctp_packet_build(p, pkt, PEER_PORT, p->remote_port, p->remote_tag,
                              ack, sizeof(ack));
    sctp_packet_send(p, pkt, (size_t)n);
    if (!p->sctp_ready) {
        p->sctp_ready = true;
        printk("webrtc: SCTP association established\n");
    }
}

static void handle_sack(Peer *p, const uint8_t *body, size_t len)
{
    if (len < 12) {
        return;
    }
    uint32_t cum = rd32(body);
    if (!p->tx_busy) {
        return;
    }
    if (cum < p->tx_base) {
        return;
    }
    uint32_t done = cum - p->tx_base + 1;
    if (done > p->tx_frags) {
        done = p->tx_frags;
    }
    if (done > p->tx_done) {
        p->tx_done = (uint16_t)done;
    }
    if (p->tx_done >= p->tx_frags) {
        p->tx_busy = false;
        k_sem_give(&p->wake);
    }
}

static void sctp_chunk(Peer *p, uint8_t type, uint8_t flags, const uint8_t *body,
                       size_t len)
{
    switch (type) {
    case SCTP_INIT:
        handle_init(p, body, len);
        break;
    case SCTP_COOKIE_ECHO:
        handle_cookie_echo(p, body, len);
        break;
    case SCTP_DATA:
        if (len < 12) {
            break;
        }
        rx_mark_tsn(p, rd32(body));
        sack_schedule(p);
        app_fragment(p, flags, rd32(body), rd16(body + 4), rd16(body + 6),
                     rd32(body + 8), body + 12, len - 12);
        break;
    case SCTP_SACK:
        handle_sack(p, body, len);
        break;
    case SCTP_HEARTBEAT: {
        uint8_t out[SCTP_PKT_MAX];
        size_t pos = 0;
        out[pos++] = SCTP_HEARTBEAT_ACK;
        out[pos++] = 0;
        wr16(out + pos, (uint16_t)(4 + len));
        pos += 2;
        if (pos + len <= sizeof(out)) {
            memcpy(out + pos, body, len);
            pos += len;
        }
        uint8_t pkt[SCTP_PKT_MAX];
        int n = sctp_packet_build(p, pkt, PEER_PORT, p->remote_port, p->remote_tag,
                                  out, pos);
        sctp_packet_send(p, pkt, (size_t)n);
        break;
    }
    case SCTP_SHUTDOWN: {
        uint8_t ack[4] = {SCTP_SHUTDOWN_ACK, 0, 0, 4};
        uint8_t pkt[SCTP_PKT_MAX];
        int n = sctp_packet_build(p, pkt, PEER_PORT, p->remote_port, p->remote_tag,
                                  ack, sizeof(ack));
        sctp_packet_send(p, pkt, (size_t)n);
        p->sctp_ready = false;
        break;
    }
    case SCTP_ABORT:
        printk("webrtc: SCTP aborted by peer\n");
        p->sctp_ready = false;
        break;
    default:
        break;
    }
}

static void sctp_input(Peer *p, const uint8_t *d, size_t n)
{
    if (n < 16) {
        return;
    }
    uint32_t vtag = rd32(d + 4);
    uint16_t src_port = rd16(d);
    if (!p->remote_port) {
        p->remote_port = src_port;
    }
    size_t pos = 12;
    while (pos + 4 <= n) {
        uint8_t type = d[pos];
        uint8_t flags = d[pos + 1];
        uint16_t len = rd16(d + pos + 2);
        if (len < 4 || pos + len > n) {
            break;
        }
        if (type == SCTP_INIT || vtag == p->local_tag) {
            sctp_chunk(p, type, flags, d + pos + 4, (size_t)len - 4);
        }
        pos += (len + 3u) & ~3u;
    }
}

static void tx_send_fragments(Peer *p)
{
    if (!p->tx_busy || !p->sctp_ready) {
        return;
    }
    for (uint16_t i = p->tx_done; i < p->tx_frags; i++) {
        size_t off = (size_t)i * SCTP_DATA_MAX;
        size_t len = p->tx_len - off;
        if (len > SCTP_DATA_MAX) {
            len = SCTP_DATA_MAX;
        }
        uint8_t flags = 0;
        if (i == 0) {
            flags |= F_BIT;
        }
        if (i == p->tx_frags - 1) {
            flags |= E_BIT;
        }
        send_data(p, 0, p->tx_ssn, STRING_PPID, p->tx_base + i, p->tx + off,
                  len, flags);
    }
    p->tx_last = k_uptime_get();
    if (p->tx_attempts < 255) {
        p->tx_attempts++;
    }
}

// --- thread -----------------------------------------------------------------

static void peer_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);
    Peer *p = &g_peer;
    printk("webrtc: peer thread running\n");

    while (!p->stop) {
        k_sem_take(&p->wake, K_MSEC(5));
        if (p->stop) {
            break;
        }

        turn_service(p);

        if (!p->tls_ready) {
            int rc = mbedtls_ssl_handshake(&p->ssl);
            if (rc == 0) {
                p->tls_ready = true;
                printk("webrtc: DTLS handshake complete\n");
            } else if (rc == MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED) {
                // We sent a HelloVerifyRequest; restart the handshake state so
                // the client's second ClientHello (with the cookie) is accepted.
                mbedtls_ssl_session_reset(&p->ssl);
                if (p->have_cur) {
                    (void)mbedtls_ssl_set_client_transport_id(
                        &p->ssl, (const unsigned char *)&p->cur, sizeof(p->cur));
                }
                mbedtls_ssl_set_timer_cb(&p->ssl, p, dtls_timer_set, dtls_timer_get);
                mbedtls_ssl_set_datagram_packing(&p->ssl, 0);
            } else if (rc != MBEDTLS_ERR_SSL_WANT_READ &&
                       rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
                printk("webrtc: DTLS handshake error -0x%04x\n", -rc);
                k_sleep(K_MSEC(50));
            }
            continue;
        }

        uint8_t buf[1536];
        int rc = mbedtls_ssl_read(&p->ssl, buf, sizeof(buf));
        if (rc > 0) {
            sctp_input(p, buf, (size_t)rc);
        } else if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            printk("webrtc: DTLS closed by peer\n");
            break;
        }

        if (p->sack_pending && k_uptime_get() >= p->sack_at) {
            send_sack(p);
        }

        if (p->tx_busy && p->sctp_ready) {
            int64_t now = k_uptime_get();
            if (p->tx_last == 0) {
                tx_send_fragments(p);
            } else if (now - p->tx_last >= p->tx_rto) {
                if (p->tx_attempts >= 8) {
                    printk("webrtc: SCTP send timed out\n");
                    p->tx_busy = false;
                } else {
                    p->tx_rto = p->tx_rto * 2 > 4000 ? 4000 : p->tx_rto * 2;
                    tx_send_fragments(p);
                }
            }
        }
    }

    printk("webrtc: peer thread exit\n");
    p->exited = true;
    k_sem_give(&p->exited_sem);
}

// --- public API -------------------------------------------------------------

void mraf_peer_set_rx(mraf_peer_rx_cb cb)
{
    g_peer.rx_cb = cb;
}

bool mraf_peer_available(void)
{
    return true;
}

bool mraf_peer_open(void)
{
    Peer *p = &g_peer;
    return p->bound && p->tls_ready && p->sctp_ready && p->dcep_acked && !p->stop;
}

void mraf_peer_add_candidate(const char *candidate)
{
    Peer *p = &g_peer;
    if (!candidate || !p->bound || !p->turn_allocated) {
        return;
    }
    struct sockaddr_in a;
    if (!candidate_addr(candidate, &a)) {
        return;
    }
    turn_queue(p, &a);
    k_sem_give(&p->wake);
}

void mraf_peer_ice_info(struct mraf_ice_info *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    Peer *p = &g_peer;
    out->stun_ok = p->have_srflx;
    out->turn_allocated = p->turn_allocated;
    out->relay_path = p->relay_used;
    if (p->have_srflx) {
        snprintf(out->srflx_addr, sizeof(out->srflx_addr), "%s:%u", p->srflx_ip,
                 p->srflx_port);
    }
    if (p->have_relay) {
        snprintf(out->relay_addr, sizeof(out->relay_addr), "%s:%u", p->relay_ip,
                 p->relay_port);
    }
    if (p->turn_error[0]) {
        snprintf(out->turn_error, sizeof(out->turn_error), "%s", p->turn_error);
    }
}

static void peer_teardown(Peer *p)
{
    if (p->tls_setup) {
        mbedtls_ssl_free(&p->ssl);
        mbedtls_ssl_config_free(&p->conf);
        mbedtls_x509_crt_free(&p->cert);
        mbedtls_pk_free(&p->key);
        p->tls_setup = false;
    }
    if (p->fd >= 0) {
        zsock_close(p->fd);
        p->fd = -1;
    }
    p->bound = false;
    p->tls_ready = false;
    p->sctp_ready = false;
    p->dcep_open = false;
    p->dcep_acked = false;
    p->have_cur = false;
    p->remote_validated = false;
    p->tx_busy = false;
    p->sack_pending = false;
    p->turn_allocated = false;
    p->have_relay = false;
    p->relay_path = false;
    p->relay_used = false;
    p->turn_nonce_len = 0;
    p->turn_realm[0] = '\0';
    p->turn_error[0] = '\0';
    p->turn_cand_head = 0;
    p->turn_cand_count = 0;
    p->turn_perm_count = 0;
}

void mraf_peer_stop(void)
{
    Peer *p = &g_peer;
    if (!p->bound && !p->tls_setup && p->fd <= 0) {
        return;
    }
    p->stop = true;
    k_sem_give(&p->wake);
    if (!p->exited) {
        k_sem_take(&p->exited_sem, K_MSEC(1000));
    }
    p->stop = false;
    p->exited = false;
    // Release the relay allocation so an immediate reconnect can allocate the
    // same 5-tuple again (otherwise the server answers 437 Allocation
    // Mismatch until its 10-minute lifetime expires).
    if (p->turn_allocated && p->fd > 0) {
        (void)turn_deallocate(p);
    }
    peer_teardown(p);
}

static bool build_answer(Peer *p, const struct Offer *o, const uint8_t fp[32],
                         char *out, size_t cap)
{
    char fpstr[100];
    fmt_fingerprint(fp, fpstr, sizeof(fpstr));
    int n = snprintf(
        out, cap,
        "v=0\r\n"
        "o=- 1 2 IN IP4 0.0.0.0\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "a=group:BUNDLE 0\r\n"
        "a=msid-semantic: WMS\r\n"
        "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=mid:0\r\n"
        "a=ice-ufrag:%s\r\n"
        "a=ice-pwd:%s\r\n"
        "a=ice-lite\r\n"
        "a=fingerprint:sha-256 %s\r\n"
        "a=setup:passive\r\n"
        "a=sctp-port:%u\r\n"
        "a=max-message-size:65536\r\n"
        "a=candidate:1 1 udp 2130706431 %s %u typ host generation 0\r\n",
        p->local_ufrag, p->local_pwd, fpstr, PEER_PORT, p->host_ip, PEER_PORT);
    if (n < 0 || (size_t)n >= cap) {
        return false;
    }
    if (p->have_srflx) {
        int m = snprintf(out + n, cap - (size_t)n,
                         "a=candidate:2 1 udp 1694498815 %s %u typ srflx "
                         "raddr %s rport %u generation 0\r\n",
                         p->srflx_ip, p->srflx_port, p->host_ip, PEER_PORT);
        if (m < 0 || (size_t)m >= cap - (size_t)n) {
            return false;
        }
        n += m;
    }
    if (p->have_relay) {
        // Lowest priority: browsers pick host/srflx when they work and fall
        // back to the relay when they do not.
        int m = snprintf(out + n, cap - (size_t)n,
                         "a=candidate:3 1 udp 16777215 %s %u typ relay "
                         "raddr %s rport %u generation 0\r\n",
                         p->relay_ip, p->relay_port, p->host_ip, PEER_PORT);
        if (m < 0 || (size_t)m >= cap - (size_t)n) {
            return false;
        }
        n += m;
    }
    (void)o;
    return true;
}

bool mraf_peer_start(const char *offer_sdp, char *answer_sdp, size_t answer_cap)
{
    Peer *p = &g_peer;
    struct Offer offer;

    if (!offer_sdp || !parse_offer(offer_sdp, &offer)) {
        printk("webrtc: cannot parse offer\n");
        return false;
    }

    mraf_peer_stop();
    mraf_peer_rx_cb keep_rx = p->rx_cb;
    memset(p, 0, sizeof(*p));
    p->rx_cb = keep_rx;
    p->fd = -1;
    p->remote_port = offer.sctp_port;
    k_sem_init(&p->wake, 0, 1);
    k_sem_init(&p->exited_sem, 0, 1);
    k_mutex_init(&p->tx_lock);
    k_mutex_init(&p->turn_lock);
    rng_seed();

    snprintf(p->remote_ufrag, sizeof(p->remote_ufrag), "%s", offer.ufrag);
    snprintf(p->remote_pwd, sizeof(p->remote_pwd), "%s", offer.pwd);
    snprintf(p->remote_fp, sizeof(p->remote_fp), "%s", offer.fp);
    snprintf(p->turn_url, sizeof(p->turn_url), "%s", g_cfg.webrtc.turn);
    snprintf(p->turn_user, sizeof(p->turn_user), "%s", g_cfg.webrtc.turn_user);
    snprintf(p->turn_pass, sizeof(p->turn_pass), "%s", g_cfg.webrtc.turn_pass);

    // Local ICE credentials and SCTP tags.
    static const char alnum[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (size_t i = 0; i < 8; i++) {
        p->local_ufrag[i] = alnum[rng_next() % (sizeof(alnum) - 1)];
    }
    p->local_ufrag[8] = '\0';
    for (size_t i = 0; i < 24; i++) {
        p->local_pwd[i] = alnum[rng_next() % (sizeof(alnum) - 1)];
    }
    p->local_pwd[24] = '\0';
    p->local_tag = rng_next();
    p->local_tsn = rng_next();

    p->fd = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (p->fd < 0) {
        printk("webrtc: socket failed (%d)\n", -errno);
        return false;
    }
    int one = 1;
    zsock_setsockopt(p->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons(PEER_PORT);
    local.sin_addr.s_addr = 0;
    if (zsock_bind(p->fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        printk("webrtc: bind %u failed (%d)\n", PEER_PORT, -errno);
        zsock_close(p->fd);
        p->fd = -1;
        return false;
    }
    zsock_fcntl(p->fd, F_SETFL, O_NONBLOCK);
    p->bound = true;

    // Candidate gathering: host address plus an optional STUN srflx. The
    // lookup runs on the peer socket so the mapped port is the real one.
    bool net_up = false;
    mraf_platform_net_info(&net_up, p->host_ip, sizeof(p->host_ip), NULL, 0, NULL);
    if (!p->host_ip[0] || strcmp(p->host_ip, "0.0.0.0") == 0) {
        snprintf(p->host_ip, sizeof(p->host_ip), "127.0.0.1");
    }
    if (g_cfg.webrtc.stun[0]) {
        p->have_srflx = stun_probe(p, g_cfg.webrtc.stun, p->srflx_ip,
                                   sizeof(p->srflx_ip), &p->srflx_port);
        if (p->have_srflx) {
            printk("webrtc: srflx candidate %s:%u\n", p->srflx_ip, p->srflx_port);
        } else {
            printk("webrtc: STUN lookup failed, trying TURN fallback\n");
        }
    }
    // TURN is advertised alongside host/srflx. It is the fallback when the
    // direct path is blocked (symmetric NAT, slirp, relay-only browser), and it
    // also creates the permissions needed for relayed browser checks.
    if (p->turn_url[0] && turn_setup(p)) {
        offer_candidates(p, offer_sdp);
    }

    // DTLS identity and server configuration.
    (void)psa_crypto_init();
    mbedtls_ssl_init(&p->ssl);
    mbedtls_ssl_config_init(&p->conf);
    mbedtls_x509_crt_init(&p->cert);
    mbedtls_pk_init(&p->key);
    p->tls_setup = true;

    int rc = mbedtls_x509_crt_parse_der(&p->cert, mraf_webrtc_cert_der,
                                        mraf_webrtc_cert_der_len);
    if (rc != 0) {
        printk("webrtc: cert parse failed -0x%04x\n", -rc);
        goto fail;
    }
    rc = mbedtls_pk_parse_key(&p->key, mraf_webrtc_key_der,
                              mraf_webrtc_key_der_len, NULL, 0);
    if (rc != 0) {
        printk("webrtc: key parse failed -0x%04x\n", -rc);
        goto fail;
    }
    rc = mbedtls_ssl_config_defaults(&p->conf, MBEDTLS_SSL_IS_SERVER,
                                     MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        printk("webrtc: ssl config failed -0x%04x\n", -rc);
        goto fail;
    }
    mbedtls_ssl_conf_authmode(&p->conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_own_cert(&p->conf, &p->cert, &p->key);
    mbedtls_ssl_conf_dtls_cookies(&p->conf, dtls_cookie_write, dtls_cookie_check, p);
    rc = mbedtls_ssl_setup(&p->ssl, &p->conf);
    if (rc != 0) {
        printk("webrtc: ssl setup failed -0x%04x\n", -rc);
        goto fail;
    }
    mbedtls_ssl_set_timer_cb(&p->ssl, p, dtls_timer_set, dtls_timer_get);
    mbedtls_ssl_set_bio(&p->ssl, p, dtls_send_cb, dtls_recv_cb, NULL);
    mbedtls_ssl_set_datagram_packing(&p->ssl, 0);

    uint8_t fp[32];
    struct Sha256 sh;
    sha256_init(&sh);
    sha256_update(&sh, p->cert.raw.p, p->cert.raw.len);
    sha256_final(&sh, fp);
    if (!build_answer(p, &offer, fp, answer_sdp, answer_cap)) {
        printk("webrtc: answer SDP overflow\n");
        goto fail;
    }

    k_thread_create(&g_peer_thread, g_peer_stack, K_THREAD_STACK_SIZEOF(g_peer_stack),
                    peer_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(7), 0,
                    K_NO_WAIT);
    k_thread_name_set(&g_peer_thread, "mraf_webrtc_peer");
    printk("webrtc: peer listening on udp/%u for %s\n", PEER_PORT, p->remote_ufrag);
    return true;

fail:
    peer_teardown(p);
    return false;
}

char *mraf_peer_tx_begin(size_t *cap)
{
    Peer *p = &g_peer;
    if (!mraf_peer_open()) {
        return NULL;
    }
    // Wait briefly for a previous message to be acknowledged; the signalling
    // thread issues at most one reply per request.
    for (int i = 0; i < 400 && p->tx_busy; i++) {
        k_sleep(K_MSEC(5));
    }
    if (p->tx_busy) {
        printk("webrtc: tx busy, dropping reply\n");
        return NULL;
    }
    if (cap) {
        *cap = MSG_MAX;
    }
    return (char *)p->tx;
}

int mraf_peer_tx_end(size_t len)
{
    Peer *p = &g_peer;
    if (len == 0 || len > MSG_MAX) {
        return -1;
    }
    k_mutex_lock(&p->tx_lock, K_FOREVER);
    p->tx_len = len;
    p->tx_base = p->local_tsn;
    p->tx_frags = (uint16_t)((len + SCTP_DATA_MAX - 1) / SCTP_DATA_MAX);
    p->tx_ssn = p->out_ssn[0]++;
    p->tx_done = 0;
    p->tx_attempts = 0;
    p->tx_rto = 500;
    p->tx_last = 0;
    p->local_tsn += p->tx_frags;
    p->tx_busy = true;
    k_mutex_unlock(&p->tx_lock);
    k_sem_give(&p->wake);
    return (int)len;
}

int mraf_peer_send(const char *message)
{
    if (!message) {
        return -1;
    }
    size_t cap = 0;
    char *dst = mraf_peer_tx_begin(&cap);
    if (!dst) {
        return -1;
    }
    size_t len = strlen(message);
    if (len == 0 || len > cap) {
        return -1;
    }
    memcpy(dst, message, len);
    return mraf_peer_tx_end(len);
}
