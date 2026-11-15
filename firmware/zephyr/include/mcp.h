// On-device Model Context Protocol endpoint (JSON-RPC 2.0, JSON response mode).
// Exposes the same controller capabilities as the server-side MCP bridge so an
// agent on the LAN can drive the device directly.
#pragma once

#include <stdbool.h>

#include <ArduinoJson.h>

#ifdef __cplusplus
extern "C" {
#endif

// Handle one JSON-RPC request. Fills `response` and returns true when a reply
// is due; returns false for notifications (the caller answers 202).
bool mraf_mcp_handle(JsonDocument &request, JsonDocument &response);

#ifdef __cplusplus
}
#endif
