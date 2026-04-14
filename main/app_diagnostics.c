#include "app_diagnostics.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "app_diag";
static const char *DIAG_NAMESPACE = "app_diag";
static const char *KEY_BOOT_COUNT = "boot_count";
static const char *KEY_REBOOT_STREAK = "reboot_stk";
static const char *KEY_BOOT_OK = "boot_ok";
static const char *KEY_SESSION_ID = "session_id";
static const char *KEY_BREADCRUMB = "breadcrumb";
static const char *KEY_DETAIL = "detail";

typedef struct {
    app_diagnostics_snapshot_t snapshot;
    bool boot_completed;
    SemaphoreHandle_t lock;
} app_diagnostics_state_t;

static app_diagnostics_state_t s_diag;

static void diag_copy_text(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strlcpy(dst, src, dst_size);
}

static bool diag_lock(TickType_t timeout_ticks)
{
    if (s_diag.lock == NULL) {
        return true;
    }
    return xSemaphoreTake(s_diag.lock, timeout_ticks) == pdTRUE;
}

static void diag_unlock(void)
{
    if (s_diag.lock != NULL) {
        xSemaphoreGive(s_diag.lock);
    }
}

static esp_err_t diag_read_string(nvs_handle_t handle, const char *key, char *buffer, size_t buffer_size)
{
    size_t required = buffer_size;
    esp_err_t err = nvs_get_str(handle, key, buffer, &required);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        buffer[0] = '\0';
    }
    return err;
}

static esp_err_t diag_persist_snapshot_locked(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(DIAG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u32(handle, KEY_BOOT_COUNT, s_diag.snapshot.boot_count);
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, KEY_REBOOT_STREAK, s_diag.snapshot.reboot_streak);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, KEY_BOOT_OK, s_diag.boot_completed ? 1U : 0U);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, KEY_SESSION_ID, s_diag.snapshot.session_id);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_BREADCRUMB, s_diag.snapshot.current_breadcrumb);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_DETAIL, s_diag.snapshot.current_detail);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

static void diag_set_state_internal(const char *breadcrumb, const char *detail, bool mark_boot_stable)
{
    if (!diag_lock(portMAX_DELAY)) {
        ESP_LOGW(TAG, "Diagnostics lock unavailable");
        return;
    }

    bool changed = false;
    if (breadcrumb != NULL && breadcrumb[0] != '\0'
        && strncmp(s_diag.snapshot.current_breadcrumb, breadcrumb, sizeof(s_diag.snapshot.current_breadcrumb)) != 0) {
        diag_copy_text(s_diag.snapshot.current_breadcrumb, sizeof(s_diag.snapshot.current_breadcrumb), breadcrumb);
        changed = true;
    }

    if (detail != NULL
        && strncmp(s_diag.snapshot.current_detail, detail, sizeof(s_diag.snapshot.current_detail)) != 0) {
        diag_copy_text(s_diag.snapshot.current_detail, sizeof(s_diag.snapshot.current_detail), detail);
        changed = true;
    }

    if (detail == NULL && s_diag.snapshot.current_detail[0] != '\0') {
        s_diag.snapshot.current_detail[0] = '\0';
        changed = true;
    }

    if (mark_boot_stable && !s_diag.boot_completed) {
        s_diag.boot_completed = true;
        changed = true;
    }

    esp_err_t err = ESP_OK;
    if (changed) {
        err = diag_persist_snapshot_locked();
    }

    diag_unlock();

    if (changed && err != ESP_OK) {
        ESP_LOGW(TAG, "Persist diagnostics state failed: %s", esp_err_to_name(err));
    }
}

const char *app_diagnostics_reset_reason_str(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_UNKNOWN:
        return "unknown";
    case ESP_RST_POWERON:
        return "power_on";
    case ESP_RST_EXT:
        return "external";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "int_wdt";
    case ESP_RST_TASK_WDT:
        return "task_wdt";
    case ESP_RST_WDT:
        return "other_wdt";
    case ESP_RST_DEEPSLEEP:
        return "deep_sleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "sdio";
#ifdef ESP_RST_USB
    case ESP_RST_USB:
        return "usb";
#endif
#ifdef ESP_RST_JTAG
    case ESP_RST_JTAG:
        return "jtag";
#endif
#ifdef ESP_RST_EFUSE
    case ESP_RST_EFUSE:
        return "efuse";
#endif
#ifdef ESP_RST_PWR_GLITCH
    case ESP_RST_PWR_GLITCH:
        return "power_glitch";
#endif
#ifdef ESP_RST_CPU_LOCKUP
    case ESP_RST_CPU_LOCKUP:
        return "cpu_lockup";
#endif
    default:
        return "other";
    }
}

void app_diagnostics_init(void)
{
    if (s_diag.lock == NULL) {
        s_diag.lock = xSemaphoreCreateMutex();
        if (s_diag.lock == NULL) {
            ESP_LOGW(TAG, "Create diagnostics lock failed; continuing unlocked");
        }
    }

    uint32_t previous_boot_count = 0;
    uint32_t previous_reboot_streak = 0;
    uint32_t previous_session_id = 0;
    uint8_t previous_boot_ok = 1;
    bool have_previous_boot_count = false;

    s_diag.snapshot.reset_reason = esp_reset_reason();
    s_diag.snapshot.previous_breadcrumb[0] = '\0';
    s_diag.snapshot.previous_detail[0] = '\0';

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(DIAG_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        if (nvs_get_u32(handle, KEY_BOOT_COUNT, &previous_boot_count) == ESP_OK) {
            have_previous_boot_count = true;
        }
        nvs_get_u32(handle, KEY_REBOOT_STREAK, &previous_reboot_streak);
        nvs_get_u32(handle, KEY_SESSION_ID, &previous_session_id);
        if (nvs_get_u8(handle, KEY_BOOT_OK, &previous_boot_ok) != ESP_OK) {
            previous_boot_ok = 1;
        }
        diag_read_string(handle, KEY_BREADCRUMB, s_diag.snapshot.previous_breadcrumb, sizeof(s_diag.snapshot.previous_breadcrumb));
        diag_read_string(handle, KEY_DETAIL, s_diag.snapshot.previous_detail, sizeof(s_diag.snapshot.previous_detail));
        nvs_close(handle);
    }

    s_diag.snapshot.previous_boot_completed = previous_boot_ok != 0;
    s_diag.snapshot.boot_count = have_previous_boot_count ? (previous_boot_count + 1U) : 1U;
    s_diag.snapshot.session_id = previous_session_id + 1U;
    if (s_diag.snapshot.session_id == 0) {
        s_diag.snapshot.session_id = s_diag.snapshot.boot_count;
    }
    if (!have_previous_boot_count) {
        s_diag.snapshot.reboot_streak = 0;
    } else if (previous_boot_ok != 0) {
        s_diag.snapshot.reboot_streak = 0;
    } else {
        s_diag.snapshot.reboot_streak = previous_reboot_streak + 1U;
    }

    diag_copy_text(s_diag.snapshot.current_breadcrumb, sizeof(s_diag.snapshot.current_breadcrumb), "boot_start");
    diag_copy_text(s_diag.snapshot.current_detail, sizeof(s_diag.snapshot.current_detail),
        app_diagnostics_reset_reason_str(s_diag.snapshot.reset_reason));
    s_diag.boot_completed = false;

    err = diag_persist_snapshot_locked();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Persist diagnostics boot state failed: %s", esp_err_to_name(err));
    }
}

const app_diagnostics_snapshot_t *app_diagnostics_snapshot(void)
{
    return &s_diag.snapshot;
}

void app_diagnostics_log_boot_summary(void)
{
    const app_diagnostics_snapshot_t *snapshot = &s_diag.snapshot;
    ESP_LOGI(TAG,
        "diag_boot session=%" PRIu32 " boot_count=%" PRIu32 " reboot_streak=%" PRIu32
        " reset_reason=%s previous_boot_completed=%d previous_breadcrumb=%s previous_detail=%s",
        snapshot->session_id,
        snapshot->boot_count,
        snapshot->reboot_streak,
        app_diagnostics_reset_reason_str(snapshot->reset_reason),
        snapshot->previous_boot_completed ? 1 : 0,
        snapshot->previous_breadcrumb[0] != '\0' ? snapshot->previous_breadcrumb : "(none)",
        snapshot->previous_detail[0] != '\0' ? snapshot->previous_detail : "(none)");
}

void app_diagnostics_note_stage(const char *breadcrumb)
{
    if (breadcrumb == NULL || breadcrumb[0] == '\0') {
        return;
    }
    diag_set_state_internal(breadcrumb, NULL, false);
}

void app_diagnostics_note_failure(const char *breadcrumb, const char *detail)
{
    if (breadcrumb == NULL || breadcrumb[0] == '\0') {
        return;
    }
    diag_set_state_internal(breadcrumb, detail != NULL ? detail : "", false);
}

void app_diagnostics_mark_boot_stable(const char *breadcrumb)
{
    if (breadcrumb == NULL || breadcrumb[0] == '\0') {
        return;
    }
    diag_set_state_internal(breadcrumb, NULL, true);
}
