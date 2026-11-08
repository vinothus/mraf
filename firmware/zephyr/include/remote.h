// Remote control/telemetry exchange with the MRAF Spring Boot server over CoAP.
#pragma once

#include <stdbool.h>

#include <ArduinoJson.h>

#ifdef __cplusplus
extern "C" {
#endif

void mraf_remote_init(void);
void mraf_remote_start(void);
void mraf_remote_build_status(JsonObject rem);
bool mraf_remote_confirmed(void);
bool mraf_remote_has_pending(void);

#ifdef __cplusplus
}
#endif
