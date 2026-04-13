#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t livekit_debug_audio_init(void);
esp_err_t livekit_debug_audio_start_recording(void);
int livekit_debug_audio_on_downlink_pcm(uint8_t *data, int size, void *ctx);

#ifdef __cplusplus
}
#endif
