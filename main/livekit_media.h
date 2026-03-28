#pragma once

#include "av_render.h"
#include "esp_capture.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t livekit_media_init(void);
esp_err_t livekit_media_play_speaker_test_tone(void);
esp_err_t livekit_media_prepare_record_runtime(void);
esp_capture_handle_t livekit_media_get_capturer(void);
av_render_handle_t livekit_media_get_renderer(void);

#ifdef __cplusplus
}
#endif
