#include "app_diagnostics.h"

#include <inttypes.h>
#include <string.h>

#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "app_diag";
static const char *DIAG_NAMESPACE = "app_diag";
static const char *KEY_BOOT_COUNT = "boot_count";
static const char *KEY_REBOOT_STREAK = "reboot_stk";
static const char *KEY_BOOT_OK = "boot_ok";
static const char *KEY_SESSION_ID = "session_id";
static const char *KEY_BREADCRUMB = "breadcrumb";
static const char *KEY_DETAIL = "detail";

#define DIAG_FLUSH_TASK_STACK_SIZE 4096
#define DIAG_FLUSH_TASK_PRIORITY 4
#define DIAG_FLUSH_COALESCE_MS 150
#define DIAG_FLUSH_RETRY_DELAY_MS 1000

typedef struct {
    app_diagnostics_snapshot_t snapshot;
    bool boot_completed;
    bool persist_pending;
    TaskHandle_t flush_task;
    SemaphoreHandle_t lock;
} app_diagnostics_state_t;

static app_diagnostics_state_t s_diag;

static void diag_flush_task(void *arg);

static void diag_detect_coredump(app_diagnostics_snapshot_t *snapshot)
{
    snapshot->coredump_present = false;
    snapshot->coredump_size = 0;
    snapshot->coredump_exc_pc = 0;
    snapshot->coredump_task[0] = '\0';
    snapshot->coredump_reason[0] = '\0';

    esp_err_t err = esp_core_dump_image_check();
    if (err == ESP_ERR_NOT_FOUND) {
        return;
    }
    if (err != ESP_OK) {
        strlcpy(snapshot->coredump_reason, esp_err_to_name(err), sizeof(snapshot->coredump_reason));
        return;
    }

    snapshot->coredump_present = true;
    size_t image_addr = 0;
    size_t image_size = 0;
    if (esp_core_dump_image_get(&image_addr, &image_size) == ESP_OK) {
        snapshot->coredump_size = (uint32_t)image_size;
    }

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    if (esp_core_dump_get_panic_reason(snapshot->coredump_reason, sizeof(snapshot->coredump_reason)) != ESP_OK) {
        snapshot->coredump_reason[0] = '\0';
    }

    esp_core_dump_summary_t summary = {};
    if (esp_core_dump_get_summary(&summary) == ESP_OK) {
        strlcpy(snapshot->coredump_task, summary.exc_task, sizeof(snapshot->coredump_task));
        snapshot->coredump_exc_pc = summary.exc_pc;
    }
#endif
}

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

static void diag_request_persist_locked(void)
{
    s_diag.persist_pending = true;
    if (s_diag.flush_task != NULL) {
        xTaskNotifyGive(s_diag.flush_task);
    }
}

static void diag_ensure_flush_task(void)
{
    if (s_diag.flush_task != NULL) {
        return;
    }

    BaseType_t task_ok = xTaskCreateWithCaps(
        diag_flush_task,
        "diag_flush",
        DIAG_FLUSH_TASK_STACK_SIZE,
        NULL,
        DIAG_FLUSH_TASK_PRIORITY,
        &s_diag.flush_task,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (task_ok != pdPASS) {
        s_diag.flush_task = NULL;
        ESP_LOGW(TAG, "Start diagnostics flush task failed");
    }
}

static void diag_flush_task(void *arg)
{
    (void)arg;

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (true) {
            vTaskDelay(pdMS_TO_TICKS(DIAG_FLUSH_COALESCE_MS));

            if (!diag_lock(portMAX_DELAY)) {
                ESP_LOGW(TAG, "Diagnostics lock unavailable during flush");
                break;
            }

            if (!s_diag.persist_pending) {
                diag_unlock();
                break;
            }

            s_diag.persist_pending = false;
            esp_err_t err = diag_persist_snapshot_locked();
            bool retry_needed = err != ESP_OK;
            if (retry_needed) {
                s_diag.persist_pending = true;
            }
            bool still_pending = s_diag.persist_pending;
            diag_unlock();

            if (retry_needed) {
                ESP_LOGW(TAG, "Persist diagnostics state failed: %s", esp_err_to_name(err));
                vTaskDelay(pdMS_TO_TICKS(DIAG_FLUSH_RETRY_DELAY_MS));
                continue;
            }

            if (!still_pending) {
                break;
            }
        }
    }
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

    if (changed) {
        diag_request_persist_locked();
    }

    diag_unlock();
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
    diag_ensure_flush_task();

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
    diag_detect_coredump(&s_diag.snapshot);
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

void app_diagnostics_copy_snapshot(app_diagnostics_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL) {
        return;
    }

    if (!diag_lock(portMAX_DELAY)) {
        memset(out_snapshot, 0, sizeof(*out_snapshot));
        return;
    }

    memcpy(out_snapshot, &s_diag.snapshot, sizeof(*out_snapshot));
    diag_unlock();
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

    if (snapshot->coredump_present) {
        ESP_LOGW(TAG,
            "diag_coredump present=1 size=%" PRIu32 " task=%s exc_pc=0x%08" PRIx32 " panic_reason=%s",
            snapshot->coredump_size,
            snapshot->coredump_task[0] != '\0' ? snapshot->coredump_task : "(unknown)",
            snapshot->coredump_exc_pc,
            snapshot->coredump_reason[0] != '\0' ? snapshot->coredump_reason : "(unknown)");
    } else if (snapshot->coredump_reason[0] != '\0') {
        ESP_LOGW(TAG, "diag_coredump present=0 state=%s", snapshot->coredump_reason);
    }
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
