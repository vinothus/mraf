// Portable DTLS/SCTP data-channel transport for the WebRTC answerer.
//
// The browser creates the peer connection and posts the offer; this module
// parses it, gathers host/srflx candidates, builds the answer SDP and drives
// an ICE-lite STUN responder, a DTLS 1.2 server (mbedTLS) and a minimal SCTP
// association (DATA/SACK/DCEP) on top. Every WebRTC application message is a
// JSON document exchanged with protocol id 51 on stream 0.
//
// The implementation only uses Zephyr's zsock API and mbedTLS, so the exact
// same code runs on native_sim (host sockets) and on the ESP32 target.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*mraf_peer_rx_cb)(const char *message);

struct mraf_ice_info;

bool mraf_peer_available(void);
bool mraf_peer_start(const char *offer_sdp, char *answer_sdp, size_t answer_cap);
void mraf_peer_stop(void);
bool mraf_peer_open(void);
int mraf_peer_send(const char *message);
/* Zero-copy replies: reserve the tx buffer, serialize into it, then commit. */
char *mraf_peer_tx_begin(size_t *cap);
int mraf_peer_tx_end(size_t len);
void mraf_peer_set_rx(mraf_peer_rx_cb cb);
/* Add a trickled browser candidate (creates a TURN permission when needed). */
void mraf_peer_add_candidate(const char *candidate);
/* Snapshot the ICE/TURN state for the dashboard. */
void mraf_peer_ice_info(struct mraf_ice_info *out);

#ifdef __cplusplus
}
#endif
