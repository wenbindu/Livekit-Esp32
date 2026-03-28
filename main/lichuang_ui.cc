#include "lichuang_ui.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_random.h"
#include "font_awesome.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lichuang_audio_board.h"
#include "lvgl.h"
#include "sdkconfig.h"

static const char *TAG = "lichuang_ui";

extern "C" {
LV_FONT_DECLARE(font_noto_basic_16_4);
LV_FONT_DECLARE(font_awesome_16_4);
LV_FONT_DECLARE(font_awesome_30_4);
LV_IMAGE_DECLARE(emoji_1f636_64); // neutral
LV_IMAGE_DECLARE(emoji_1f642_64); // happy
LV_IMAGE_DECLARE(emoji_1f606_64); // laughing
LV_IMAGE_DECLARE(emoji_1f614_64); // sad
LV_IMAGE_DECLARE(emoji_1f609_64); // winking
LV_IMAGE_DECLARE(emoji_1f60e_64); // cool
LV_IMAGE_DECLARE(emoji_1f61c_64); // silly
LV_IMAGE_DECLARE(emoji_1f60d_64); // loving
}

namespace {

constexpr int kUiWidth = 320;
constexpr int kUiHeight = 240;

#ifndef CONFIG_LK_EXAMPLE_UI_BUFFER_LINES
#define CONFIG_LK_EXAMPLE_UI_BUFFER_LINES 12
#endif

#ifndef CONFIG_LK_EXAMPLE_UI_TRANSFER_LINES
#define CONFIG_LK_EXAMPLE_UI_TRANSFER_LINES 6
#endif

#ifndef CONFIG_LK_EXAMPLE_UI_LVGL_TASK_STACK
#define CONFIG_LK_EXAMPLE_UI_LVGL_TASK_STACK 4096
#endif

constexpr int kDisplayBufferLines = CONFIG_LK_EXAMPLE_UI_BUFFER_LINES;
constexpr int kDisplayTransferLines = CONFIG_LK_EXAMPLE_UI_TRANSFER_LINES;
constexpr int kDisplayMaxTransferLines =
    (kDisplayTransferLines > kDisplayBufferLines) ? kDisplayTransferLines : kDisplayBufferLines;

#if CONFIG_SPIRAM && CONFIG_LK_EXAMPLE_UI_USE_PSRAM_BUFFER
constexpr bool kUsePsramDisplayBuffer = true;
#else
constexpr bool kUsePsramDisplayBuffer = false;
#endif

constexpr spi_host_device_t kSpiHost = SPI3_HOST;
constexpr gpio_num_t kSpiMosi = GPIO_NUM_40;
constexpr gpio_num_t kSpiSclk = GPIO_NUM_41;
constexpr gpio_num_t kLcdDc = GPIO_NUM_39;
constexpr gpio_num_t kBacklightPin = GPIO_NUM_42;
constexpr bool kBacklightInvert = true;

struct ui_state_t {
    bool initialized = false;
    bool animation_enabled = false;
    esp_lcd_panel_io_handle_t panel_io = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;
    lv_display_t *display = nullptr;
    lv_obj_t *top_bar = nullptr;
    lv_obj_t *network_icon = nullptr;
    lv_obj_t *audio_icon = nullptr;
    lv_obj_t *status_bar = nullptr;
    lv_obj_t *title_label = nullptr;
    lv_obj_t *emoji_box = nullptr;
    lv_obj_t *emoji_icon = nullptr;
    lv_obj_t *emoji_image = nullptr;
    lv_obj_t *headline_label = nullptr;
    lv_obj_t *bottom_bar = nullptr;
    lv_obj_t *detail_label = nullptr;
    lv_timer_t *emoji_timer = nullptr;
    size_t online_emoji_index = 0;
    char title[48] = {};
    char line1[192] = {};
    char line2[208] = {};
    char emoji[16] = {};
};

ui_state_t s_ui;

const char *const kOnlineEmojis[] = {
    ":)",
    ":D",
    ";)",
    ":P",
    "B)",
    "<3",
};

void set_backlight_on()
{
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << kBacklightPin;
    cfg.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(gpio_set_level(kBacklightPin, kBacklightInvert ? 0 : 1));
}

void fill_panel_white(esp_lcd_panel_handle_t panel)
{
    uint16_t row_buffer[kUiWidth];
    for (int x = 0; x < kUiWidth; ++x) {
        row_buffer[x] = 0xFFFF;
    }

    for (int y = 0; y < kUiHeight; ++y) {
        esp_lcd_panel_draw_bitmap(panel, 0, y, kUiWidth, y + 1, row_buffer);
    }
}

esp_err_t panel_disp_on(esp_lcd_panel_handle_t panel)
{
    esp_err_t err = esp_lcd_panel_disp_on_off(panel, true);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Panel disp_on_off is not supported, assuming display is on");
        return ESP_OK;
    }
    return err;
}

const lv_image_dsc_t *emoji_from_ascii(const char *emoji)
{
    if (emoji == nullptr || emoji[0] == '\0') {
        return &emoji_1f636_64;
    }
    if (strcmp(emoji, ":)") == 0) {
        return &emoji_1f642_64;
    }
    if (strcmp(emoji, ":D") == 0) {
        return &emoji_1f606_64;
    }
    if (strcmp(emoji, ":(") == 0) {
        return &emoji_1f614_64;
    }
    if (strcmp(emoji, ":|") == 0) {
        return &emoji_1f636_64;
    }
    if (strcmp(emoji, ";)") == 0) {
        return &emoji_1f609_64;
    }
    if (strcmp(emoji, "B)") == 0) {
        return &emoji_1f60e_64;
    }
    if (strcmp(emoji, ":P") == 0) {
        return &emoji_1f61c_64;
    }
    if (strcmp(emoji, "<3") == 0) {
        return &emoji_1f60d_64;
    }
    return &emoji_1f636_64;
}

void set_label_text_or_space(lv_obj_t *label, const char *text)
{
    lv_label_set_text(label, (text != nullptr && text[0] != '\0') ? text : " ");
}

void apply_emoji_locked(const char *emoji)
{
    const lv_image_dsc_t *image = emoji_from_ascii(emoji);
    if (image != nullptr) {
        lv_image_set_src(s_ui.emoji_image, image);
        lv_obj_add_flag(s_ui.emoji_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_ui.emoji_image, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_label_set_text(s_ui.emoji_icon, FONT_AWESOME_MICROCHIP_AI);
    lv_obj_add_flag(s_ui.emoji_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_ui.emoji_icon, LV_OBJ_FLAG_HIDDEN);
}

void refresh_ui_locked()
{
    bool show_detail = s_ui.line2[0] != '\0';

    set_label_text_or_space(s_ui.title_label, s_ui.title);
    set_label_text_or_space(s_ui.headline_label, s_ui.line1);
    set_label_text_or_space(s_ui.detail_label, s_ui.line2);
    apply_emoji_locked(s_ui.emoji);

    if (!show_detail) {
        lv_obj_add_flag(s_ui.bottom_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_ui.bottom_bar, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_align(s_ui.emoji_box, LV_ALIGN_CENTER, 0, show_detail ? -56 : -26);
    lv_obj_align_to(s_ui.headline_label, s_ui.emoji_box, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
    lv_obj_align_to(s_ui.bottom_bar, s_ui.headline_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
}

void emoji_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (!s_ui.animation_enabled) {
        return;
    }

    s_ui.online_emoji_index = (s_ui.online_emoji_index + 1) %
        (sizeof(kOnlineEmojis) / sizeof(kOnlineEmojis[0]));
    strlcpy(s_ui.emoji, kOnlineEmojis[s_ui.online_emoji_index], sizeof(s_ui.emoji));
    apply_emoji_locked(s_ui.emoji);
}

esp_err_t create_display_hw()
{
    ESP_RETURN_ON_ERROR(lichuang_board_io_init(), TAG, "board io init failed");

    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = kSpiMosi;
    bus_cfg.miso_io_num = GPIO_NUM_NC;
    bus_cfg.sclk_io_num = kSpiSclk;
    bus_cfg.quadwp_io_num = GPIO_NUM_NC;
    bus_cfg.quadhd_io_num = GPIO_NUM_NC;
    bus_cfg.max_transfer_sz = kUiWidth * kUiHeight * sizeof(uint16_t);

    esp_err_t err = spi_bus_initialize(kSpiHost, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = GPIO_NUM_NC;
    io_config.dc_gpio_num = kLcdDc;
    io_config.spi_mode = 2;
    io_config.pclk_hz = 80 * 1000 * 1000;
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(kSpiHost, &io_config, &s_ui.panel_io), TAG, "create panel io failed");

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = GPIO_NUM_NC;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_ui.panel_io, &panel_config, &s_ui.panel), TAG, "create panel failed");

    esp_lcd_panel_reset(s_ui.panel);
    ESP_RETURN_ON_ERROR(lichuang_board_set_output_state(0, 0), TAG, "enable display rail failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_ui.panel), TAG, "init panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_ui.panel, true), TAG, "invert panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_ui.panel, true), TAG, "swap xy failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_ui.panel, true, false), TAG, "mirror panel failed");
    ESP_RETURN_ON_ERROR(panel_disp_on(s_ui.panel), TAG, "turn panel on failed");

    fill_panel_white(s_ui.panel);
    set_backlight_on();
    return ESP_OK;
}

esp_err_t create_lvgl_display()
{
    if (!lv_is_initialized()) {
        lv_init();
    }

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port init failed");

    lvgl_port_display_cfg_t display_cfg = {};
    display_cfg.io_handle = s_ui.panel_io;
    display_cfg.panel_handle = s_ui.panel;
    display_cfg.control_handle = nullptr;
    display_cfg.buffer_size = kUiWidth * kDisplayBufferLines;
    display_cfg.double_buffer = false;
    display_cfg.trans_size = kUsePsramDisplayBuffer ? (kUiWidth * kDisplayTransferLines) : 0;
    display_cfg.hres = kUiWidth;
    display_cfg.vres = kUiHeight;
    display_cfg.monochrome = false;
    display_cfg.rotation.swap_xy = true;
    display_cfg.rotation.mirror_x = true;
    display_cfg.rotation.mirror_y = false;
    display_cfg.color_format = LV_COLOR_FORMAT_RGB565;
    display_cfg.flags.buff_dma = kUsePsramDisplayBuffer ? 0 : 1;
    display_cfg.flags.buff_spiram = kUsePsramDisplayBuffer ? 1 : 0;
    display_cfg.flags.sw_rotate = 0;
    display_cfg.flags.swap_bytes = 1;
    display_cfg.flags.full_refresh = 0;
    display_cfg.flags.direct_mode = 0;

    ESP_LOGI(TAG,
        "LVGL buffer lines=%d trans_lines=%d psram=%d task_stack=internal",
        kDisplayBufferLines,
        0,
        0);

    s_ui.display = lvgl_port_add_disp(&display_cfg);
    ESP_RETURN_ON_FALSE(s_ui.display != nullptr, ESP_FAIL, TAG, "add lvgl display failed");
    return ESP_OK;
}

void create_ui_locked()
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0x111827), 0);
    lv_obj_set_style_text_font(screen, &font_noto_basic_16_4, 0);

    s_ui.top_bar = lv_obj_create(screen);
    lv_obj_remove_flag(s_ui.top_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_ui.top_bar, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(s_ui.top_bar, 0, 0);
    lv_obj_set_style_bg_color(s_ui.top_bar, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s_ui.top_bar, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_ui.top_bar, 0, 0);
    lv_obj_set_style_pad_top(s_ui.top_bar, 8, 0);
    lv_obj_set_style_pad_bottom(s_ui.top_bar, 8, 0);
    lv_obj_set_style_pad_left(s_ui.top_bar, 16, 0);
    lv_obj_set_style_pad_right(s_ui.top_bar, 16, 0);
    lv_obj_set_flex_flow(s_ui.top_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_ui.top_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(s_ui.top_bar, LV_ALIGN_TOP_MID, 0, 0);

    s_ui.network_icon = lv_label_create(s_ui.top_bar);
    lv_obj_set_style_text_font(s_ui.network_icon, &font_awesome_16_4, 0);
    lv_obj_set_style_text_color(s_ui.network_icon, lv_color_hex(0x111827), 0);
    lv_label_set_text(s_ui.network_icon, FONT_AWESOME_WIFI);

    s_ui.audio_icon = lv_label_create(s_ui.top_bar);
    lv_obj_set_style_text_font(s_ui.audio_icon, &font_awesome_16_4, 0);
    lv_obj_set_style_text_color(s_ui.audio_icon, lv_color_hex(0x111827), 0);
    lv_label_set_text(s_ui.audio_icon, FONT_AWESOME_MICROPHONE);

    s_ui.status_bar = lv_obj_create(screen);
    lv_obj_remove_flag(s_ui.status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_ui.status_bar, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(s_ui.status_bar, 0, 0);
    lv_obj_set_style_bg_opa(s_ui.status_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ui.status_bar, 0, 0);
    lv_obj_set_style_pad_top(s_ui.status_bar, 8, 0);
    lv_obj_set_style_pad_bottom(s_ui.status_bar, 8, 0);
    lv_obj_set_style_pad_all(s_ui.status_bar, 0, 0);
    lv_obj_align(s_ui.status_bar, LV_ALIGN_TOP_MID, 0, 0);

    s_ui.title_label = lv_label_create(s_ui.status_bar);
    lv_obj_set_width(s_ui.title_label, LV_HOR_RES * 3 / 4);
    lv_obj_set_style_text_font(s_ui.title_label, &font_noto_basic_16_4, 0);
    lv_obj_set_style_text_color(s_ui.title_label, lv_color_hex(0x111827), 0);
    lv_obj_set_style_text_align(s_ui.title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_ui.title_label, LV_LABEL_LONG_CLIP);
    lv_obj_align(s_ui.title_label, LV_ALIGN_CENTER, 0, 0);

    s_ui.emoji_box = lv_obj_create(screen);
    lv_obj_remove_flag(s_ui.emoji_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_ui.emoji_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_ui.emoji_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ui.emoji_box, 0, 0);
    lv_obj_set_style_pad_all(s_ui.emoji_box, 0, 0);

    s_ui.emoji_icon = lv_label_create(s_ui.emoji_box);
    lv_obj_set_style_text_font(s_ui.emoji_icon, &font_awesome_30_4, 0);
    lv_obj_set_style_text_color(s_ui.emoji_icon, lv_color_hex(0x111827), 0);
    lv_label_set_text(s_ui.emoji_icon, FONT_AWESOME_MICROCHIP_AI);
    lv_obj_center(s_ui.emoji_icon);

    s_ui.emoji_image = lv_image_create(s_ui.emoji_box);
    lv_obj_center(s_ui.emoji_image);
    lv_obj_add_flag(s_ui.emoji_image, LV_OBJ_FLAG_HIDDEN);

    s_ui.headline_label = lv_label_create(screen);
    lv_obj_set_width(s_ui.headline_label, LV_HOR_RES - 56);
    lv_obj_set_style_text_font(s_ui.headline_label, &font_noto_basic_16_4, 0);
    lv_obj_set_style_text_color(s_ui.headline_label, lv_color_hex(0x111827), 0);
    lv_obj_set_style_text_align(s_ui.headline_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_line_space(s_ui.headline_label, 2, 0);
    lv_label_set_long_mode(s_ui.headline_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_ui.headline_label, LV_ALIGN_TOP_MID, 0, 122);
    lv_label_set_text(s_ui.headline_label, " ");

    s_ui.bottom_bar = lv_obj_create(screen);
    lv_obj_remove_flag(s_ui.bottom_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_ui.bottom_bar, LV_HOR_RES - 24, 66);
    lv_obj_set_style_radius(s_ui.bottom_bar, 18, 0);
    lv_obj_set_style_bg_color(s_ui.bottom_bar, lv_color_hex(0xF3F4F6), 0);
    lv_obj_set_style_bg_opa(s_ui.bottom_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ui.bottom_bar, 0, 0);
    lv_obj_set_style_pad_left(s_ui.bottom_bar, 16, 0);
    lv_obj_set_style_pad_right(s_ui.bottom_bar, 16, 0);
    lv_obj_set_style_pad_top(s_ui.bottom_bar, 8, 0);
    lv_obj_set_style_pad_bottom(s_ui.bottom_bar, 8, 0);
    lv_obj_align(s_ui.bottom_bar, LV_ALIGN_BOTTOM_MID, 0, -10);

    s_ui.detail_label = lv_label_create(s_ui.bottom_bar);
    lv_obj_set_width(s_ui.detail_label, LV_HOR_RES - 64);
    lv_obj_set_style_text_font(s_ui.detail_label, &font_noto_basic_16_4, 0);
    lv_obj_set_style_text_color(s_ui.detail_label, lv_color_hex(0x4B5563), 0);
    lv_obj_set_style_text_align(s_ui.detail_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_line_space(s_ui.detail_label, 2, 0);
    lv_label_set_long_mode(s_ui.detail_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_ui.detail_label, " ");
    lv_obj_align(s_ui.detail_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_ui.bottom_bar, LV_OBJ_FLAG_HIDDEN);

    s_ui.emoji_timer = lv_timer_create(emoji_timer_cb, 4000, nullptr);
    if (s_ui.emoji_timer != nullptr) {
        lv_timer_pause(s_ui.emoji_timer);
    }
}

void stop_animation_locked()
{
    s_ui.animation_enabled = false;
    if (s_ui.emoji_timer != nullptr) {
        lv_timer_pause(s_ui.emoji_timer);
    }
}

void cache_message_state(const char *title, const char *line1, const char *line2, const char *emoji, bool animate)
{
    strlcpy(s_ui.title, title != nullptr ? title : "", sizeof(s_ui.title));
    strlcpy(s_ui.line1, line1 != nullptr ? line1 : "", sizeof(s_ui.line1));
    strlcpy(s_ui.line2, line2 != nullptr ? line2 : "", sizeof(s_ui.line2));
    strlcpy(s_ui.emoji, emoji != nullptr ? emoji : ":|", sizeof(s_ui.emoji));
    s_ui.animation_enabled = animate;
}

void apply_cached_message_locked()
{
    refresh_ui_locked();

    if (!s_ui.animation_enabled) {
        stop_animation_locked();
        return;
    }

    s_ui.animation_enabled = true;
    s_ui.online_emoji_index = esp_random() % (sizeof(kOnlineEmojis) / sizeof(kOnlineEmojis[0]));
    strlcpy(s_ui.emoji, kOnlineEmojis[s_ui.online_emoji_index], sizeof(s_ui.emoji));
    apply_emoji_locked(s_ui.emoji);
    if (s_ui.emoji_timer != nullptr) {
        lv_timer_resume(s_ui.emoji_timer);
        lv_timer_ready(s_ui.emoji_timer);
    }
}

bool lock_ui()
{
    return s_ui.initialized && lvgl_port_lock(5000);
}

void unlock_ui()
{
    lvgl_port_unlock();
}

} // namespace

extern "C" esp_err_t lichuang_ui_init(void)
{
    if (s_ui.initialized) {
        return ESP_OK;
    }

    if (s_ui.panel == nullptr || s_ui.panel_io == nullptr) {
        esp_err_t hw_err = ESP_FAIL;
        for (int attempt = 1; attempt <= 5; ++attempt) {
            hw_err = create_display_hw();
            if (hw_err == ESP_OK) {
                break;
            }
            ESP_LOGW(TAG, "Display HW init attempt %d/5 failed: %s", attempt, esp_err_to_name(hw_err));
            vTaskDelay(pdMS_TO_TICKS(60));
        }
        ESP_RETURN_ON_ERROR(hw_err, TAG, "display hw init failed");
    }

    ESP_RETURN_ON_ERROR(create_lvgl_display(), TAG, "lvgl display init failed");

    if (!lvgl_port_lock(5000)) {
        return ESP_ERR_TIMEOUT;
    }

    create_ui_locked();
    if (s_ui.title[0] == '\0') {
        cache_message_state("LIVEKIT", "BOOTING DEVICE", "LICHUANG ESP32-S3", ":)", false);
    }
    apply_cached_message_locked();
    unlock_ui();

    s_ui.initialized = true;
    ESP_LOGI(TAG, "Lichuang LVGL UI initialized");
    return ESP_OK;
}

extern "C" esp_err_t lichuang_ui_suspend(void)
{
    if (!s_ui.initialized) {
        return ESP_OK;
    }

    if (lock_ui()) {
        stop_animation_locked();
        unlock_ui();
    }

    esp_err_t stop_err = lvgl_port_stop();
    if (stop_err != ESP_OK && stop_err != ESP_ERR_INVALID_STATE) {
        return stop_err;
    }
    vTaskDelay(pdMS_TO_TICKS(40));
    ESP_LOGI(TAG, "Lichuang LVGL UI suspended");
    return ESP_OK;
}

extern "C" esp_err_t lichuang_ui_resume(void)
{
    if (!s_ui.initialized) {
        return lichuang_ui_init();
    }

    esp_err_t err = lvgl_port_resume();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    if (lock_ui()) {
        apply_cached_message_locked();
        unlock_ui();
    }

    ESP_LOGI(TAG, "Lichuang LVGL UI resumed");
    return ESP_OK;
}

extern "C" void lichuang_ui_stop_animation(void)
{
    if (!lock_ui()) {
        return;
    }
    stop_animation_locked();
    unlock_ui();
}

extern "C" void lichuang_ui_show_message(const char *title, const char *line1, const char *line2, const char *emoji)
{
    cache_message_state(title, line1, line2, emoji, false);
    if (!lock_ui()) {
        return;
    }
    apply_cached_message_locked();
    unlock_ui();
}

extern "C" void lichuang_ui_show_boot_prompt(int remaining_seconds)
{
    char line2[48];
    snprintf(line2, sizeof(line2), "AUTO CONNECT IN %dS", remaining_seconds);
    lichuang_ui_show_message("PRESS BOOT", "FOR WIFI SETUP", line2, ";)");
}

extern "C" void lichuang_ui_show_online(const char *title, const char *line1, const char *line2)
{
    cache_message_state(title, line1, line2, ":)", true);
    if (!lock_ui()) {
        return;
    }
    apply_cached_message_locked();
    unlock_ui();
}
