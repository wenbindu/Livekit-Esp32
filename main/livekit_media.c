#include "livekit_media.h"

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "av_render_default.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_enc_default.h"
#include "esp_capture_defaults.h"
#include "esp_capture_audio_dev_src.h"
#include "esp_capture_sink.h"
#include "esp_check.h"
#include "esp_log.h"

#include "livekit_debug_audio.h"
#include "lichuang_audio_board.h"

static const char *TAG = "livekit_media";

#ifndef CONFIG_LK_EXAMPLE_AUDIO_USE_AEC
#define CONFIG_LK_EXAMPLE_AUDIO_USE_AEC 0
#endif

#ifndef CONFIG_LK_EXAMPLE_AUDIO_MIC_CHANNEL
#define CONFIG_LK_EXAMPLE_AUDIO_MIC_CHANNEL 0
#endif

#ifndef CONFIG_LK_EXAMPLE_AUDIO_REF_CHANNEL
#define CONFIG_LK_EXAMPLE_AUDIO_REF_CHANNEL 1
#endif

#ifndef CONFIG_LK_EXAMPLE_AUDIO_INPUT_GAIN_DB
#define CONFIG_LK_EXAMPLE_AUDIO_INPUT_GAIN_DB 37
#endif

#ifndef CONFIG_LK_EXAMPLE_AUDIO_DATA_ON_VAD
#define CONFIG_LK_EXAMPLE_AUDIO_DATA_ON_VAD 0
#endif

#ifndef CONFIG_LK_EXAMPLE_AUDIO_RENDER_RAW_FIFO_KB
#define CONFIG_LK_EXAMPLE_AUDIO_RENDER_RAW_FIFO_KB 16
#endif

#ifndef CONFIG_LK_EXAMPLE_AUDIO_RENDER_FIFO_KB
#define CONFIG_LK_EXAMPLE_AUDIO_RENDER_FIFO_KB 24
#endif

typedef struct {
    esp_capture_sink_handle_t capturer_handle;
    esp_capture_audio_src_if_t *audio_source;
} capture_system_t;

typedef struct {
    audio_render_handle_t audio_renderer;
    av_render_handle_t av_renderer_handle;
} renderer_system_t;

static capture_system_t s_capturer_system;
static renderer_system_t s_renderer_system;
static bool s_media_initialized;

static uint16_t capture_bit(uint8_t channel)
{
    return ESP_CODEC_DEV_MAKE_CHANNEL_MASK(channel);
}

static const char *aec_layout_string(uint8_t mic_channel, uint8_t ref_channel)
{
    if (mic_channel == ref_channel) {
        return "M";
    }
    if (mic_channel < ref_channel) {
        return "MR";
    }
    return "RM";
}

static esp_err_t play_pcm_frame(const int16_t *samples, size_t sample_count)
{
    esp_codec_dev_handle_t playback_handle = lichuang_audio_board_get_playback_handle();
    ESP_RETURN_ON_FALSE(playback_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "playback handle not ready");
    ESP_RETURN_ON_FALSE(
        esp_codec_dev_write(playback_handle, (void *)samples, (int)(sample_count * sizeof(int16_t))) == ESP_CODEC_DEV_OK,
        ESP_FAIL,
        TAG,
        "write test tone frame failed");
    return ESP_OK;
}

static esp_err_t play_sine_segment(uint32_t sample_rate, uint32_t duration_ms, float frequency_hz, int16_t amplitude)
{
    const size_t frame_samples = (sample_rate / 1000) * 20;
    const size_t total_samples = (sample_rate * duration_ms) / 1000;
    const size_t ramp_samples = (sample_rate * 25) / 1000;
    const float phase_step = (2.0f * (float)M_PI * frequency_hz) / (float)sample_rate;
    float phase = 0.0f;
    int16_t frame[frame_samples];

    for (size_t offset = 0; offset < total_samples; offset += frame_samples) {
        size_t chunk_samples = total_samples - offset;
        if (chunk_samples > frame_samples) {
            chunk_samples = frame_samples;
        }

        for (size_t i = 0; i < chunk_samples; ++i) {
            const size_t sample_index = offset + i;
            float gain = 1.0f;
            if (sample_index < ramp_samples) {
                gain = sinf(((float)sample_index / (float)ramp_samples) * ((float)M_PI * 0.5f));
            } else if (sample_index + ramp_samples > total_samples) {
                const size_t release_index = total_samples - sample_index;
                gain = sinf(((float)release_index / (float)ramp_samples) * ((float)M_PI * 0.5f));
            }

            frame[i] = (int16_t)(sinf(phase) * (float)amplitude * gain);
            phase += phase_step;
            if (phase > (2.0f * (float)M_PI)) {
                phase -= (2.0f * (float)M_PI);
            }
        }

        ESP_RETURN_ON_ERROR(play_pcm_frame(frame, chunk_samples), TAG, "play sine segment failed");
    }

    return ESP_OK;
}

static esp_err_t play_silence_segment(uint32_t sample_rate, uint32_t duration_ms)
{
    const size_t frame_samples = (sample_rate / 1000) * 20;
    const size_t total_samples = (sample_rate * duration_ms) / 1000;
    int16_t frame[frame_samples];
    memset(frame, 0, sizeof(frame));

    for (size_t offset = 0; offset < total_samples; offset += frame_samples) {
        size_t chunk_samples = total_samples - offset;
        if (chunk_samples > frame_samples) {
            chunk_samples = frame_samples;
        }
        ESP_RETURN_ON_ERROR(play_pcm_frame(frame, chunk_samples), TAG, "play silence failed");
    }

    return ESP_OK;
}

static void livekit_capture_scheduler(const char *thread_name, esp_capture_thread_schedule_cfg_t *schedule_cfg)
{
    if (strcmp(thread_name, "buffer_in") == 0) {
        schedule_cfg->stack_size = 10 * 1024;
        schedule_cfg->priority = 10;
        schedule_cfg->core_id = 0;
        schedule_cfg->stack_in_ext = 1;
    } else if (strcmp(thread_name, "AUD_SRC") == 0) {
        // AFE init has already completed before the steady-state source task starts.
        // Keep this stack in PSRAM so debug features don't starve internal RAM.
        schedule_cfg->stack_size = 8 * 1024;
        schedule_cfg->priority = 15;
        schedule_cfg->core_id = 0;
        schedule_cfg->stack_in_ext = 1;
    } else if (strcmp(thread_name, "aenc_0") == 0) {
        schedule_cfg->stack_size = 40 * 1024;
        schedule_cfg->priority = 2;
        schedule_cfg->core_id = 1;
        schedule_cfg->stack_in_ext = 1;
    }
}

static esp_err_t build_capturer_system(void)
{
    esp_codec_dev_handle_t record_handle = lichuang_audio_board_get_record_handle();
    ESP_RETURN_ON_FALSE(record_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "record handle not ready");

    const uint8_t mic_channel = CONFIG_LK_EXAMPLE_AUDIO_MIC_CHANNEL;
    const uint16_t mic_mask = capture_bit(mic_channel);
    const float input_gain_db = (float)CONFIG_LK_EXAMPLE_AUDIO_INPUT_GAIN_DB;

    ESP_RETURN_ON_ERROR(
        livekit_media_prepare_record_runtime(),
        TAG,
        "prepare record for capture failed");

#if CONFIG_LK_EXAMPLE_AUDIO_USE_AEC
    const uint8_t ref_channel = CONFIG_LK_EXAMPLE_AUDIO_REF_CHANNEL;
    const uint16_t record_mask = (uint16_t)(mic_mask | capture_bit(ref_channel));
    const char *mic_layout = aec_layout_string(mic_channel, ref_channel);

    esp_capture_audio_aec_src_cfg_t codec_cfg = {
        .mic_layout = mic_layout,
        .record_handle = record_handle,
        .channel = 4,
        .channel_mask = record_mask,
        .data_on_vad = CONFIG_LK_EXAMPLE_AUDIO_DATA_ON_VAD,
    };
    ESP_LOGI(TAG,
        "Capture mode=AEC mic_channel=%u ref_channel=%u mic_layout=%s record_mask=0x%x gain_db=%.1f data_on_vad=%d",
        mic_channel,
        ref_channel,
        mic_layout,
        record_mask,
        (double)input_gain_db,
        CONFIG_LK_EXAMPLE_AUDIO_DATA_ON_VAD);
    s_capturer_system.audio_source = esp_capture_new_audio_aec_src(&codec_cfg);
    ESP_RETURN_ON_FALSE(s_capturer_system.audio_source != NULL, ESP_ERR_NO_MEM, TAG, "create AEC source failed");
#else
    esp_capture_audio_dev_src_cfg_t codec_cfg = {
        .record_handle = record_handle,
    };
    ESP_LOGI(TAG,
        "Capture mode=RAW mic_channel=%u record_mask=0x%x gain_db=%.1f",
        mic_channel,
        mic_mask,
        (double)input_gain_db);
    s_capturer_system.audio_source = esp_capture_new_audio_dev_src(&codec_cfg);
    ESP_RETURN_ON_FALSE(s_capturer_system.audio_source != NULL, ESP_ERR_NO_MEM, TAG, "create raw capture source failed");
#endif

    esp_capture_cfg_t capture_cfg = {
        .sync_mode = ESP_CAPTURE_SYNC_MODE_AUDIO,
        .audio_src = s_capturer_system.audio_source,
    };
    esp_capture_open(&capture_cfg, &s_capturer_system.capturer_handle);
    ESP_RETURN_ON_FALSE(s_capturer_system.capturer_handle != NULL, ESP_ERR_NO_MEM, TAG, "open capturer failed");
    return ESP_OK;
}

static esp_err_t build_renderer_system(void)
{
    esp_codec_dev_handle_t playback_handle = lichuang_audio_board_get_playback_handle();
    ESP_RETURN_ON_FALSE(playback_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "playback handle not ready");

    i2s_render_cfg_t i2s_cfg = {
        .play_handle = playback_handle,
        .cb = livekit_debug_audio_on_downlink_pcm,
        .ctx = NULL,
    };
    s_renderer_system.audio_renderer = av_render_alloc_i2s_render(&i2s_cfg);
    ESP_RETURN_ON_FALSE(s_renderer_system.audio_renderer != NULL, ESP_ERR_NO_MEM, TAG, "create renderer failed");

    ESP_RETURN_ON_ERROR(
        esp_codec_dev_set_out_vol(playback_handle, CONFIG_LK_EXAMPLE_SPEAKER_VOLUME),
        TAG,
        "set speaker volume failed");

    const uint32_t audio_raw_fifo_size = (uint32_t)CONFIG_LK_EXAMPLE_AUDIO_RENDER_RAW_FIFO_KB * 1024U;
    const uint32_t audio_render_fifo_size = (uint32_t)CONFIG_LK_EXAMPLE_AUDIO_RENDER_FIFO_KB * 1024U;
    ESP_LOGI(TAG,
        "Renderer FIFO raw=%" PRIu32 "KB render=%" PRIu32 "KB",
        (uint32_t)CONFIG_LK_EXAMPLE_AUDIO_RENDER_RAW_FIFO_KB,
        (uint32_t)CONFIG_LK_EXAMPLE_AUDIO_RENDER_FIFO_KB);

    av_render_cfg_t render_cfg = {
        .audio_render = s_renderer_system.audio_renderer,
        .audio_raw_fifo_size = audio_raw_fifo_size,
        .audio_render_fifo_size = audio_render_fifo_size,
        .allow_drop_data = false,
    };
    s_renderer_system.av_renderer_handle = av_render_open(&render_cfg);
    ESP_RETURN_ON_FALSE(s_renderer_system.av_renderer_handle != NULL, ESP_ERR_NO_MEM, TAG, "open av_render failed");

    av_render_audio_frame_info_t frame_info = {
        .sample_rate = 16000,
        .channel = 1,
        .bits_per_sample = 16,
    };
    av_render_set_fixed_frame_info(s_renderer_system.av_renderer_handle, &frame_info);
    return ESP_OK;
}

esp_err_t livekit_media_init(void)
{
    if (s_media_initialized) {
        return ESP_OK;
    }

    esp_audio_enc_register_default();
    esp_audio_dec_register_default();
    esp_capture_set_thread_scheduler(livekit_capture_scheduler);

    ESP_RETURN_ON_ERROR(build_capturer_system(), TAG, "build capturer failed");
    ESP_RETURN_ON_ERROR(build_renderer_system(), TAG, "build renderer failed");

    s_media_initialized = true;
    ESP_LOGI(TAG, "Media pipeline initialized");
    return ESP_OK;
}

esp_err_t livekit_media_prepare_record_runtime(void)
{
    const uint8_t mic_channel = CONFIG_LK_EXAMPLE_AUDIO_MIC_CHANNEL;
    const uint16_t mic_mask = capture_bit(mic_channel);
    const float input_gain_db = (float)CONFIG_LK_EXAMPLE_AUDIO_INPUT_GAIN_DB;

#if CONFIG_LK_EXAMPLE_AUDIO_USE_AEC
    const uint8_t ref_channel = CONFIG_LK_EXAMPLE_AUDIO_REF_CHANNEL;
    const uint16_t record_mask = (uint16_t)(mic_mask | capture_bit(ref_channel));
    ESP_LOGI(TAG,
        "Reprepare record runtime for AEC: mic_channel=%u ref_channel=%u record_mask=0x%x gain_db=%.1f",
        mic_channel,
        ref_channel,
        record_mask,
        (double)input_gain_db);
    return lichuang_audio_board_prepare_record(16000, 4, record_mask, mic_mask, input_gain_db);
#else
    ESP_LOGI(TAG,
        "Reprepare record runtime for RAW: mic_channel=%u record_mask=0x%x gain_db=%.1f",
        mic_channel,
        mic_mask,
        (double)input_gain_db);
    return lichuang_audio_board_prepare_record(16000, 4, mic_mask, mic_mask, input_gain_db);
#endif
}

esp_capture_handle_t livekit_media_get_capturer(void)
{
    return s_capturer_system.capturer_handle;
}

av_render_handle_t livekit_media_get_renderer(void)
{
    return s_renderer_system.av_renderer_handle;
}

esp_err_t livekit_media_play_speaker_test_tone(void)
{
    ESP_RETURN_ON_FALSE(s_media_initialized, ESP_ERR_INVALID_STATE, TAG, "media not initialized");
    ESP_RETURN_ON_FALSE(s_renderer_system.av_renderer_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "renderer not ready");

    const uint32_t sample_rate = 16000;
    ESP_LOGI(TAG, "Playing speaker self-test chime");

    ESP_RETURN_ON_ERROR(play_sine_segment(sample_rate, 140, 523.25f, 4200), TAG, "play first chime failed");
    ESP_RETURN_ON_ERROR(play_silence_segment(sample_rate, 50), TAG, "play pause failed");
    ESP_RETURN_ON_ERROR(play_sine_segment(sample_rate, 180, 659.25f, 4600), TAG, "play second chime failed");

    return ESP_OK;
}
