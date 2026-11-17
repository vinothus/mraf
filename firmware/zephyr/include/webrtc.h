// WebRTC peer-to-peer link with the browser dashboard.
//
// The device is always the *answerer*: the browser creates the data channel,
// posts an SDP offer to the server, and the device answers. This module is the
// control plane only - it talks HTTP to the server's signalling endpoints
// (GET /api/webrtc/config, GET/POST /api/webrtc/signal, POST /api/webrtc/report)
// and dispatches the JSON commands that arrive on the data channel.
//
// The DTLS/SCTP transport itself lives behind the mraf_platform_webrtc_* calls
// so the same signalling and dispatch logic is shared by the ESP32 target and
// the native_sim test build (which has no transport and reports unavailable).
#pragma once

#include <stdbool.h>

#include <ArduinoJson.h>

#ifdef __cplusplus
extern "C" {
#endif

void mraf_webrtc_init(void);
void mraf_webrtc_start(void);

// Forget the cached control-plane config so the next poll re-reads
// GET /api/webrtc/config. Called when the operator saves new WebRTC settings.
void mraf_webrtc_config_changed(void);

// Describe the peer link for the /api/status document (also what the server
// sees in the CoAP status payload).
void mraf_webrtc_build_status(JsonObject out);

#ifdef __cplusplus
}
#endif
