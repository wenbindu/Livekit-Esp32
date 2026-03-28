#include "wifi_connect.h"

#include <string>
#include <cstdio>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"

#include "lichuang_ui.h"
#include "ssid_manager.h"
#include "wifi_manager.h"

namespace {

constexpr EventBits_t WIFI_CONNECTED_BIT = BIT0;
constexpr EventBits_t WIFI_CONFIG_EXIT_BIT = BIT1;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 60000;
constexpr uint32_t WIFI_FORCE_AP_WINDOW_MS = 2500;
constexpr uint32_t WIFI_FORCE_AP_POLL_MS = 25;
constexpr gpio_num_t BOOT_BUTTON_GPIO = GPIO_NUM_0;

const char *TAG = "wifi_connect";

void init_boot_button()
{
    static bool initialized = false;
    if (initialized) {
        return;
    }

    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&config);
    initialized = true;
}

bool boot_button_pressed()
{
    init_boot_button();
    return gpio_get_level(BOOT_BUTTON_GPIO) == 0;
}

bool wait_for_force_provisioning_request()
{
    int press_streak = 0;
    int previous_second = -1;
    const int total_steps = WIFI_FORCE_AP_WINDOW_MS / WIFI_FORCE_AP_POLL_MS;

    for (int step = 0; step < total_steps; ++step) {
        int remaining_ms = static_cast<int>(WIFI_FORCE_AP_WINDOW_MS - (step * WIFI_FORCE_AP_POLL_MS));
        int remaining_seconds = (remaining_ms + 999) / 1000;
        if (remaining_seconds != previous_second) {
            previous_second = remaining_seconds;
            lichuang_ui_show_boot_prompt(remaining_seconds);
        }

        if (boot_button_pressed()) {
            press_streak++;
            if (press_streak >= 2) {
                lichuang_ui_show_message("WI-FI SETUP", "BOOT BUTTON DETECTED", "STARTING PORTAL", ":D");
                vTaskDelay(pdMS_TO_TICKS(150));
                return true;
            }
        } else {
            press_streak = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(WIFI_FORCE_AP_POLL_MS));
    }

    return false;
}

void clear_wait_bits(EventGroupHandle_t events)
{
    xEventGroupClearBits(events, WIFI_CONNECTED_BIT | WIFI_CONFIG_EXIT_BIT);
}

void register_wifi_callback(EventGroupHandle_t events)
{
    auto &wifi = WifiManager::GetInstance();
    wifi.SetEventCallback([events](WifiEvent event, const std::string &) {
        // WifiManager callback runs on the Wi-Fi event task. Keep this path
        // minimal to avoid exhausting the event task stack.
        switch (event) {
        case WifiEvent::Connected:
            xEventGroupSetBits(events, WIFI_CONNECTED_BIT);
            break;
        case WifiEvent::ConfigModeExit:
            xEventGroupSetBits(events, WIFI_CONFIG_EXIT_BIT);
            break;
        case WifiEvent::Scanning:
        case WifiEvent::Connecting:
        case WifiEvent::Disconnected:
        case WifiEvent::ConfigModeEnter:
            break;
        }
    });
}

bool wait_for_station_connected(EventGroupHandle_t events, uint32_t timeout_ms)
{
    EventBits_t bits = xEventGroupWaitBits(
        events,
        WIFI_CONNECTED_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool start_station_and_wait(EventGroupHandle_t events)
{
    auto &wifi = WifiManager::GetInstance();
    lichuang_ui_show_message("WI-FI", "CONNECTING", "USING SAVED NETWORK", ":|");
    clear_wait_bits(events);
    wifi.StartStation();
    if (!wait_for_station_connected(events, WIFI_CONNECT_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "Timed out waiting for saved Wi-Fi credentials");
        lichuang_ui_show_message("WI-FI", "CONNECT FAILED", "OPENING SETUP PORTAL", ":(");
        wifi.StopStation();
        return false;
    }

    char ip_line[48];
    std::snprintf(ip_line, sizeof(ip_line), "IP %s", wifi.GetIpAddress().c_str());
    lichuang_ui_show_message("WI-FI CONNECTED", wifi.GetSsid().c_str(), ip_line, "B)");
    ESP_LOGI(
        TAG,
        "Using SSID: %s, IP: %s",
        wifi.GetSsid().c_str(),
        wifi.GetIpAddress().c_str());
    return true;
}

bool start_provisioning_portal_and_wait(EventGroupHandle_t events)
{
    auto &wifi = WifiManager::GetInstance();

    while (true) {
        clear_wait_bits(events);
        wifi.StartConfigAp();

        ESP_LOGW(
            TAG,
            "Open provisioning portal: AP=%s URL=%s",
            wifi.GetApSsid().c_str(),
            wifi.GetApWebUrl().c_str());
        lichuang_ui_show_message("WI-FI SETUP", wifi.GetApSsid().c_str(), "OPEN 192.168.4.1", ":D");

        EventBits_t bits = xEventGroupWaitBits(
            events,
            WIFI_CONFIG_EXIT_BIT,
            pdTRUE,
            pdFALSE,
            portMAX_DELAY);
        if ((bits & WIFI_CONFIG_EXIT_BIT) == 0) {
            continue;
        }

        if (SsidManager::GetInstance().GetSsidList().empty()) {
            ESP_LOGW(TAG, "No Wi-Fi credentials were saved, keeping provisioning portal available");
            lichuang_ui_show_message("WI-FI SETUP", wifi.GetApSsid().c_str(), "SAVE WIFI TO CONTINUE", ":D");
            continue;
        }

#if CONFIG_LK_EXAMPLE_WIFI_PROVISIONING_REBOOT_ON_SUCCESS
        ESP_LOGI(TAG, "Provisioning completed, rebooting to restart with saved Wi-Fi");
        lichuang_ui_show_message("WI-FI SAVED", "RESTARTING DEVICE", "RECONNECTING SOON", ";)");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
#endif

        if (start_station_and_wait(events)) {
            return true;
        }

        ESP_LOGW(TAG, "Provisioned Wi-Fi failed, restarting provisioning portal");
    }
}

} // namespace

extern "C" bool lk_example_network_connect(void)
{
    if (!CONFIG_LK_EXAMPLE_USE_WIFI) {
        ESP_LOGE(TAG, "Only Wi-Fi is supported in this project");
        return false;
    }

    EventGroupHandle_t events = xEventGroupCreate();
    if (events == nullptr) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi event group");
        return false;
    }

    auto cleanup = [events](bool result) {
        vEventGroupDelete(events);
        return result;
    };

    auto &wifi = WifiManager::GetInstance();
    if (!wifi.IsInitialized()) {
        WifiManagerConfig config;
#if CONFIG_LK_EXAMPLE_WIFI_SETUP_PROVISIONING
        config.ssid_prefix = CONFIG_LK_EXAMPLE_WIFI_AP_SSID_PREFIX;
        config.language = CONFIG_LK_EXAMPLE_WIFI_AP_LANGUAGE;
#endif
        if (!wifi.Initialize(config)) {
            ESP_LOGE(TAG, "Failed to initialize Wi-Fi manager");
            return cleanup(false);
        }
    }

    esp_err_t ui_err = lichuang_ui_init();
    if (ui_err != ESP_OK) {
        ESP_LOGW(TAG, "Display init failed: %s", esp_err_to_name(ui_err));
    }

    register_wifi_callback(events);

#if CONFIG_LK_EXAMPLE_WIFI_SETUP_FIXED
    if (strlen(CONFIG_LK_EXAMPLE_WIFI_SSID) == 0) {
        ESP_LOGE(TAG, "Missing CONFIG_LK_EXAMPLE_WIFI_SSID");
        return cleanup(false);
    }

    SsidManager::GetInstance().AddSsid(CONFIG_LK_EXAMPLE_WIFI_SSID, CONFIG_LK_EXAMPLE_WIFI_PASSWORD);
    return cleanup(start_station_and_wait(events));
#elif CONFIG_LK_EXAMPLE_WIFI_SETUP_PROVISIONING
    const bool has_saved_credentials = !SsidManager::GetInstance().GetSsidList().empty();
#if CONFIG_LK_EXAMPLE_WIFI_PROVISIONING_FORCE_AP
    const bool force_ap_on_boot = true;
#else
    const bool force_ap_on_boot = false;
#endif
    const bool force_ap_from_button = has_saved_credentials && !force_ap_on_boot
        ? wait_for_force_provisioning_request()
        : false;

    if (!force_ap_on_boot && !force_ap_from_button && has_saved_credentials) {
        if (start_station_and_wait(events)) {
            return cleanup(true);
        }
        ESP_LOGW(TAG, "Saved Wi-Fi connection failed, falling back to provisioning portal");
    }

    if (force_ap_from_button) {
        ESP_LOGI(TAG, "BOOT button requested provisioning portal");
    }

    return cleanup(start_provisioning_portal_and_wait(events));
#else
    ESP_LOGE(TAG, "Unsupported Wi-Fi setup mode");
    return cleanup(false);
#endif
}
