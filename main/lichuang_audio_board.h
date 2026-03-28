#pragma once

#include <stdint.h>

#include "esp_codec_dev.h"
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t lichuang_board_io_init(void);
esp_err_t lichuang_board_set_output_state(uint8_t bit, uint8_t level);
i2c_master_bus_handle_t lichuang_board_get_i2c_bus(void);
esp_err_t lichuang_audio_board_init(void);
esp_err_t lichuang_audio_board_prepare_record(uint32_t sample_rate, uint8_t total_channels, uint16_t channel_mask, uint16_t gain_mask, float gain_db);
esp_codec_dev_handle_t lichuang_audio_board_get_record_handle(void);
esp_codec_dev_handle_t lichuang_audio_board_get_playback_handle(void);

#ifdef __cplusplus
}
#endif
