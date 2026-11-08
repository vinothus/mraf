#pragma once

#include <Arduino.h>
#include <stddef.h>

// Minimal CoAP confirmable POST client for a JSON endpoint.
// Returns the CoAP response code (for example 68 = 2.04 Changed) on success,
// or a negative value on transport/parse failure. The response payload, if
// any, is copied to `response` and NUL-terminated.
int coapPost(const char *host, uint16_t port, const char *path,
             const char *payload, char *response, size_t response_cap,
             unsigned long timeout_ms);
