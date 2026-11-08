// Persistent configuration blob (Zephyr settings/NVS on ESP32, file on
// native_sim).
#pragma once

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CONFIG_MRAF_SETTINGS_JSON_MAX
#define CONFIG_MRAF_SETTINGS_JSON_MAX 8192
#endif

bool mraf_storage_load(char *buf, size_t cap);
bool mraf_storage_save(const char *json);
void mraf_storage_init(void);

#ifdef __cplusplus
}
#endif
