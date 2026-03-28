#include <stdbool.h>
#include <sys/time.h>
#include <time.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "lichuang_audio_board.h"
#include "livekit_debug_audio.h"
#include "lichuang_ui.h"
#include "livekit.h"
#include "livekit_app.h"
#include "livekit_media.h"
#include "wifi_connect.h"

#ifndef CONFIG_LK_EXAMPLE_TIME_SYNC_WAIT_MS
#define CONFIG_LK_EXAMPLE_TIME_SYNC_WAIT_MS 1200
#endif

static const char *TAG = "livekit_esp32s3";
static const int TIME_SYNC_POLL_MS = 500;
static const int TIME_SYNC_BACKGROUND_POLL_MS = 1000;
static const int TIME_SYNC_BACKGROUND_TIMEOUT_MS = 20000;
static const char *TIME_CACHE_NAMESPACE = "clock_state";
static const char *TIME_CACHE_KEY_EPOCH = "epoch";
static const time_t MIN_REASONABLE_EPOCH = 1704067200; // 2024-01-01 UTC
static const gpio_num_t CHAT_BUTTON_GPIO = GPIO_NUM_0;
static const uint32_t APP_TASK_STACK_SIZE = 16384;
static const UBaseType_t APP_TASK_PRIORITY = 5;
static bool s_sntp_started;
static bool s_time_sync_task_started;

static void init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

static bool clock_is_reasonable(void)
{
    time_t now = 0;
    struct tm timeinfo = {0};
    time(&now);
    localtime_r(&now, &timeinfo);
    return timeinfo.tm_year >= (2024 - 1900);
}

static void cache_current_time_if_valid(void)
{
    if (!clock_is_reasonable()) {
        return;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(TIME_CACHE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Open time cache for write failed: %s", esp_err_to_name(err));
        return;
    }

    time_t now = 0;
    time(&now);
    err = nvs_set_i64(handle, TIME_CACHE_KEY_EPOCH, (int64_t)now);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Persist cached time failed: %s", esp_err_to_name(err));
    }
    nvs_close(handle);
}

static bool restore_cached_time(void)
{
    if (clock_is_reasonable()) {
        return true;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(TIME_CACHE_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    int64_t cached_epoch = 0;
    err = nvs_get_i64(handle, TIME_CACHE_KEY_EPOCH, &cached_epoch);
    nvs_close(handle);
    if (err != ESP_OK || cached_epoch < (int64_t)MIN_REASONABLE_EPOCH) {
        return false;
    }

    struct timeval tv = {
        .tv_sec = (time_t)cached_epoch,
        .tv_usec = 0,
    };
    if (settimeofday(&tv, NULL) != 0) {
        ESP_LOGW(TAG, "Restore cached time failed");
        return false;
    }

    ESP_LOGI(TAG, "Restored cached time and continuing with background SNTP sync");
    return true;
}

static void start_sntp_if_needed(void)
{
    if (s_sntp_started) {
        return;
    }

    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2, ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "ntp.tencent.com"));
    esp_netif_sntp_init(&sntp_config);
    s_sntp_started = true;
}

static void time_sync_monitor_task(void *arg)
{
    (void)arg;

    const int attempts = TIME_SYNC_BACKGROUND_TIMEOUT_MS / TIME_SYNC_BACKGROUND_POLL_MS;
    for (int i = 0; i < attempts; ++i) {
        if (clock_is_reasonable()) {
            cache_current_time_if_valid();
            ESP_LOGI(TAG, "Background time sync completed");
            s_time_sync_task_started = false;
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(TIME_SYNC_BACKGROUND_POLL_MS));
    }

    ESP_LOGW(TAG, "Background time sync timed out after %d ms", TIME_SYNC_BACKGROUND_TIMEOUT_MS);
    s_time_sync_task_started = false;
    vTaskDelete(NULL);
}

static void ensure_time_sync_monitor_task(void)
{
    if (s_time_sync_task_started) {
        return;
    }
    if (xTaskCreate(time_sync_monitor_task, "time_sync", 4096, NULL, 2, NULL) == pdPASS) {
        s_time_sync_task_started = true;
        return;
    }
    ESP_LOGW(TAG, "Create background time sync task failed");
}

static void wait_for_time_sync(void)
{
    if (clock_is_reasonable()) {
        cache_current_time_if_valid();
        ESP_LOGI(TAG, "Clock already valid, skip foreground SNTP wait");
        return;
    }

    bool restored_from_cache = restore_cached_time();
    start_sntp_if_needed();

    if (restored_from_cache) {
        ensure_time_sync_monitor_task();
        return;
    }

    const int attempts = CONFIG_LK_EXAMPLE_TIME_SYNC_WAIT_MS / TIME_SYNC_POLL_MS;
    for (int i = 0; i < attempts; ++i) {
        if (clock_is_reasonable()) {
            cache_current_time_if_valid();
            ESP_LOGI(TAG, "Foreground time synchronized");
            ensure_time_sync_monitor_task();
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(TIME_SYNC_POLL_MS));
    }

    ESP_LOGW(TAG,
        "Time sync not confirmed after %d ms, continuing anyway",
        CONFIG_LK_EXAMPLE_TIME_SYNC_WAIT_MS);
    ensure_time_sync_monitor_task();
}

static void init_chat_button(void)
{
    static bool initialized = false;
    if (initialized) {
        return;
    }

    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << CHAT_BUTTON_GPIO;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&cfg));
    initialized = true;
}

static bool chat_button_pressed(void)
{
    init_chat_button();
    return gpio_get_level(CHAT_BUTTON_GPIO) == 0;
}

static void wait_for_chat_button_press(void)
{
    bool observed_press = false;

    lichuang_ui_show_message("STANDBY", "PRESS BOOT", "TO START CHAT", ":|");
    ESP_LOGI(TAG, "Standby mode enabled; waiting for BOOT button");

    while (true) {
        bool pressed = chat_button_pressed();
        if (pressed) {
            observed_press = true;
        } else if (observed_press) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

static void app_task(void *arg)
{
    (void)arg;
    esp_log_level_set("*", ESP_LOG_INFO);

    init_nvs();
    livekit_system_init();

    if (!lk_example_network_connect()) {
        ESP_LOGE(TAG, "Wi-Fi connection failed");
        lichuang_ui_show_message("WI-FI FAILED", "CHECK NETWORK", "RESTART TO RETRY", ":(");
        return;
    }

#if CONFIG_LK_EXAMPLE_START_IN_STANDBY
    wait_for_chat_button_press();
#endif

    lichuang_ui_show_message("LIVEKIT", "CHECKING CLOCK", "BACKGROUND NTP", ":|");
    wait_for_time_sync();

    ESP_ERROR_CHECK(lichuang_audio_board_init());
    ESP_ERROR_CHECK(livekit_media_init());

    esp_err_t debug_audio_err = livekit_debug_audio_init();
    if (debug_audio_err != ESP_OK) {
        ESP_LOGW(TAG, "Debug uplink recorder disabled: %s", esp_err_to_name(debug_audio_err));
    }

#if CONFIG_LK_EXAMPLE_LOCAL_AUDIO_UPLINK_ONLY
    if (CONFIG_LK_EXAMPLE_DEBUG_UPLINK_WS_URL[0] == '\0') {
        ESP_LOGE(TAG, "Local audio uplink mode enabled but websocket URL is empty");
        lichuang_ui_show_message("LOCAL AUDIO", "WS URL MISSING", "SET DEBUG_UPLINK_WS_URL", ":(");
        return;
    }

    lichuang_ui_show_message("LOCAL AUDIO", "STREAMING TO HOST", "WS UPLINK ACTIVE", ":)");
    ESP_LOGI(TAG, "Local audio uplink only mode enabled; skipping LiveKit room join");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif

    lichuang_ui_show_message("LIVEKIT", "PREPARING AUDIO", "JOINING ROOM", ":)");
    bool join_started = livekit_app_join_room();
    if (!join_started) {
        lichuang_ui_show_message("LIVEKIT", "ROOM START FAILED", "CHECK SERIAL LOG", ":(");
    }

    vTaskDelete(NULL);
}

void app_main(void)
{
    BaseType_t task_ok = xTaskCreatePinnedToCore(
        app_task,
        "lk_app",
        APP_TASK_STACK_SIZE,
        NULL,
        APP_TASK_PRIORITY,
        NULL,
        0);
    ESP_ERROR_CHECK(task_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
