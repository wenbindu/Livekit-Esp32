#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_downlink_capture_init(void);
void app_downlink_capture_on_pcm(const uint8_t *data, size_t size);

#ifdef __cplusplus
}
#endif
