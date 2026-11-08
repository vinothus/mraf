// Low-level GPIO/millis HAL implemented per platform.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void mraf_gpio_config(uint8_t pin, uint8_t mode);
void mraf_gpio_write(uint8_t pin, uint8_t level);
int mraf_gpio_read(uint8_t pin);
unsigned long mraf_gpio_millis(void);

#ifdef __cplusplus
}
#endif
