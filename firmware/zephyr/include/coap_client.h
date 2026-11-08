// Minimal CoAP confirmable POST client for a JSON endpoint.
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int coapPost(const char *host, uint16_t port, const char *path,
             const char *payload, char *response, size_t response_cap,
             unsigned long timeout_ms);

#ifdef __cplusplus
}
#endif
