#include "lichuang_audio_board.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lichuang_board";

#define AUDIO_INPUT_SAMPLE_RATE     16000
#define AUDIO_OUTPUT_SAMPLE_RATE    16000
#define AUDIO_INPUT_REFERENCE       true

#define AUDIO_I2S_GPIO_MCLK         GPIO_NUM_38
#define AUDIO_I2S_GPIO_WS           GPIO_NUM_13
#define AUDIO_I2S_GPIO_BCLK         GPIO_NUM_14
#define AUDIO_I2S_GPIO_DIN          GPIO_NUM_12
#define AUDIO_I2S_GPIO_DOUT         GPIO_NUM_45

#define AUDIO_CODEC_I2C_PORT        I2C_NUM_1
#define AUDIO_CODEC_I2C_SDA_PIN     GPIO_NUM_1
#define AUDIO_CODEC_I2C_SCL_PIN     GPIO_NUM_2
#define AUDIO_CODEC_ES8311_ADDR     ES8311_CODEC_DEFAULT_ADDR
#define AUDIO_CODEC_ES7210_ADDR     0x82

#define PCA9557_ADDR                0x19
#define PCA9557_OUTPUT_REG          0x01
#define PCA9557_CONFIG_REG          0x03

static bool s_initialized;
static bool s_pca9557_ready;
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_pca9557_dev;
static i2s_chan_handle_t s_tx_handle;
static i2s_chan_handle_t s_rx_handle;

static const audio_codec_data_if_t *s_data_if;
static const audio_codec_ctrl_if_t *s_out_ctrl_if;
static const audio_codec_if_t *s_out_codec_if;
static const audio_codec_ctrl_if_t *s_in_ctrl_if;
static const audio_codec_if_t *s_in_codec_if;
static const audio_codec_gpio_if_t *s_gpio_if;

static esp_codec_dev_handle_t s_output_dev;
static esp_codec_dev_handle_t s_input_dev;

static esp_err_t pca9557_write_reg(uint8_t reg, uint8_t value)
{
    ESP_RETURN_ON_FALSE(s_pca9557_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "PCA9557 not initialized");
    uint8_t payload[2] = {reg, value};
    return i2c_master_transmit(s_pca9557_dev, payload, sizeof(payload), -1);
}

static esp_err_t pca9557_read_reg(uint8_t reg, uint8_t *value)
{
    ESP_RETURN_ON_FALSE(s_pca9557_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "PCA9557 not initialized");
    return i2c_master_transmit_receive(s_pca9557_dev, &reg, 1, value, 1, -1);
}

static esp_err_t pca9557_set_output_state(uint8_t bit, uint8_t level)
{
    uint8_t data = 0;
    ESP_RETURN_ON_ERROR(pca9557_read_reg(PCA9557_OUTPUT_REG, &data), TAG, "read PCA9557 failed");
    data = (uint8_t)((data & ~(1U << bit)) | ((level & 0x1U) << bit));
    return pca9557_write_reg(PCA9557_OUTPUT_REG, data);
}

static esp_err_t init_i2c(void)
{
    if (s_i2c_bus != NULL && s_pca9557_dev != NULL && s_pca9557_ready) {
        return ESP_OK;
    }

    if (s_i2c_bus == NULL) {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = AUDIO_CODEC_I2C_PORT,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_bus_cfg, &s_i2c_bus), TAG, "create i2c bus failed");
    }

    if (s_pca9557_dev == NULL) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = PCA9557_ADDR,
            .scl_speed_hz = 400000,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_pca9557_dev), TAG, "add PCA9557 failed");
    }

    esp_err_t last_err = ESP_FAIL;
    for (int attempt = 1; attempt <= 8; ++attempt) {
        last_err = pca9557_write_reg(PCA9557_OUTPUT_REG, 0x03);
        if (last_err == ESP_OK) {
            last_err = pca9557_write_reg(PCA9557_CONFIG_REG, 0xf8);
        }
        if (last_err == ESP_OK) {
            s_pca9557_ready = true;
            ESP_LOGI(TAG, "PCA9557 initialized on attempt %d", attempt);
            return ESP_OK;
        }

        s_pca9557_ready = false;
        ESP_LOGW(TAG, "PCA9557 init attempt %d/8 failed: %s", attempt, esp_err_to_name(last_err));
        vTaskDelay(pdMS_TO_TICKS(40));
    }

    return last_err;
}

static esp_err_t init_i2s(void)
{
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = 240,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_handle, &s_rx_handle), TAG, "create i2s channel failed");

    i2s_std_config_t tx_cfg = {
        .clk_cfg = {
            .sample_rate_hz = AUDIO_OUTPUT_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = AUDIO_I2S_GPIO_MCLK,
            .bclk = AUDIO_I2S_GPIO_BCLK,
            .ws = AUDIO_I2S_GPIO_WS,
            .dout = AUDIO_I2S_GPIO_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    i2s_tdm_config_t rx_cfg = {
        .clk_cfg = {
            .sample_rate_hz = AUDIO_INPUT_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            .bclk_div = 8,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = (i2s_tdm_slot_mask_t)(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
            .ws_width = I2S_TDM_AUTO_WS_WIDTH,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = false,
            .big_endian = false,
            .bit_order_lsb = false,
            .skip_mask = false,
            .total_slot = I2S_TDM_AUTO_SLOT_NUM,
        },
        .gpio_cfg = {
            .mclk = AUDIO_I2S_GPIO_MCLK,
            .bclk = AUDIO_I2S_GPIO_BCLK,
            .ws = AUDIO_I2S_GPIO_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = AUDIO_I2S_GPIO_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_handle, &tx_cfg), TAG, "init tx failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(s_rx_handle, &rx_cfg), TAG, "init rx failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_handle), TAG, "enable tx failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_handle), TAG, "enable rx failed");
    return ESP_OK;
}

static esp_err_t init_codec_devs(void)
{
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = s_rx_handle,
        .tx_handle = s_tx_handle,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(s_data_if != NULL, ESP_ERR_NO_MEM, TAG, "create data_if failed");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = AUDIO_CODEC_I2C_PORT,
        .addr = AUDIO_CODEC_ES8311_ADDR,
        .bus_handle = s_i2c_bus,
    };
    s_out_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(s_out_ctrl_if != NULL, ESP_ERR_NO_MEM, TAG, "create out ctrl_if failed");

    s_gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(s_gpio_if != NULL, ESP_ERR_NO_MEM, TAG, "create gpio_if failed");

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = s_out_ctrl_if,
        .gpio_if = s_gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = GPIO_NUM_NC,
        .use_mclk = true,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
    };
    s_out_codec_if = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(s_out_codec_if != NULL, ESP_ERR_NO_MEM, TAG, "create ES8311 failed");

    esp_codec_dev_cfg_t out_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = s_out_codec_if,
        .data_if = s_data_if,
    };
    s_output_dev = esp_codec_dev_new(&out_dev_cfg);
    ESP_RETURN_ON_FALSE(s_output_dev != NULL, ESP_ERR_NO_MEM, TAG, "create playback dev failed");

    i2c_cfg.addr = AUDIO_CODEC_ES7210_ADDR;
    s_in_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(s_in_ctrl_if != NULL, ESP_ERR_NO_MEM, TAG, "create in ctrl_if failed");

    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = s_in_ctrl_if,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
    };
    s_in_codec_if = es7210_codec_new(&es7210_cfg);
    ESP_RETURN_ON_FALSE(s_in_codec_if != NULL, ESP_ERR_NO_MEM, TAG, "create ES7210 failed");

    esp_codec_dev_cfg_t in_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = s_in_codec_if,
        .data_if = s_data_if,
    };
    s_input_dev = esp_codec_dev_new(&in_dev_cfg);
    ESP_RETURN_ON_FALSE(s_input_dev != NULL, ESP_ERR_NO_MEM, TAG, "create record dev failed");

    esp_codec_dev_sample_info_t out_fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = AUDIO_OUTPUT_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_output_dev, &out_fs), TAG, "open playback failed");
    ESP_RETURN_ON_ERROR(pca9557_set_output_state(1, 1), TAG, "enable amp failed");

    esp_codec_dev_sample_info_t in_fs = {
        .bits_per_sample = 16,
        .channel = 4,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
        .sample_rate = AUDIO_INPUT_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    if (AUDIO_INPUT_REFERENCE) {
        in_fs.channel_mask |= ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
    }
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_input_dev, &in_fs), TAG, "open record failed");
    ESP_RETURN_ON_ERROR(
        esp_codec_dev_set_in_channel_gain(s_input_dev, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), 30),
        TAG,
        "set mic gain failed");
    return ESP_OK;
}

esp_err_t lichuang_audio_board_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(lichuang_board_io_init(), TAG, "i2c init failed");
    ESP_RETURN_ON_ERROR(init_i2s(), TAG, "i2s init failed");
    ESP_RETURN_ON_ERROR(init_codec_devs(), TAG, "codec init failed");

    s_initialized = true;
    ESP_LOGI(TAG, "Lichuang audio board initialized");
    return ESP_OK;
}

esp_err_t lichuang_audio_board_prepare_record(uint32_t sample_rate, uint8_t total_channels, uint16_t channel_mask, uint16_t gain_mask, float gain_db)
{
    ESP_RETURN_ON_FALSE(s_input_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "record device not initialized");
    ESP_RETURN_ON_FALSE(total_channels > 0, ESP_ERR_INVALID_ARG, TAG, "invalid record channel count");

    esp_codec_dev_sample_info_t in_fs = {
        .bits_per_sample = 16,
        .channel = total_channels,
        .channel_mask = channel_mask,
        .sample_rate = sample_rate,
        .mclk_multiple = 0,
    };

    ESP_RETURN_ON_ERROR(esp_codec_dev_close(s_input_dev), TAG, "close previous record config failed");
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_input_dev, &in_fs), TAG, "open record with new config failed");

    if (gain_mask != 0) {
        ESP_RETURN_ON_ERROR(
            esp_codec_dev_set_in_channel_gain(s_input_dev, gain_mask, gain_db),
            TAG,
            "set record channel gain failed");
    } else {
        ESP_RETURN_ON_ERROR(esp_codec_dev_set_in_gain(s_input_dev, gain_db), TAG, "set record gain failed");
    }

    ESP_LOGI(TAG,
        "Prepared record device: rate=%" PRIu32 " total_channels=%u channel_mask=0x%x gain_mask=0x%x gain_db=%.1f",
        sample_rate,
        total_channels,
        channel_mask,
        gain_mask,
        (double)gain_db);
    return ESP_OK;
}

esp_err_t lichuang_board_io_init(void)
{
    return init_i2c();
}

esp_err_t lichuang_board_set_output_state(uint8_t bit, uint8_t level)
{
    if (s_pca9557_dev == NULL) {
        ESP_RETURN_ON_ERROR(lichuang_board_io_init(), TAG, "board io init failed");
    }
    return pca9557_set_output_state(bit, level);
}

i2c_master_bus_handle_t lichuang_board_get_i2c_bus(void)
{
    return s_i2c_bus;
}

esp_codec_dev_handle_t lichuang_audio_board_get_record_handle(void)
{
    return s_input_dev;
}

esp_codec_dev_handle_t lichuang_audio_board_get_playback_handle(void)
{
    return s_output_dev;
}
