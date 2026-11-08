#pragma once
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

void mraf_api_init(void);
void mraf_api_start(void);
uint16_t mraf_api_port(void);

#ifdef __cplusplus
}
#endif
