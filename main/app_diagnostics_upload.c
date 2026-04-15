#include "app_diagnostics_upload.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>

#include "app_diagnostics.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "app_diag_upload";

#define DIAG_EVENT_URL_MAX 256
#define DIAG_EVENT_RESPONSE_MAX 512
#define DIAG_UPLOAD_TASK_STACK_SIZE 8192
#define DIAG_UPLOAD_TASK_PRIORITY 3

typedef struct {
    char event_url[DIAG_EVENT_URL_MAX];
    app_diagnostics_snapshot_t snapshot;
} diag_upload_task_args_t;

typedef struct {
    char body[DIAG_EVENT_RESPONSE_MAX];
    size_t len;
} diag_http_response_buffer_t;

static bool s_boot_event_upload_scheduled = false;

static const char *active_participant_identity(void)
{
    return strlen(CONFIG_LK_EXAMPLE_PARTICIPANT_IDENTITY) > 0
        ? CONFIG_LK_EXAMPLE_PARTICIPANT_IDENTITY
        : CONFIG_LK_EXAMPLE_PARTICIPANT_NAME;
}

static esp_err_t diag_http_response_buffer_event_handler(esp_http_client_event_t *event)
{
    diag_http_response_buffer_t *buffer = (diag_http_response_buffer_t *)event->user_data;
    if (buffer == NULL) {
        return ESP_OK;
    }

    if (event->event_id != HTTP_EVENT_ON_DATA || event->data == NULL || event->data_len <= 0) {
        return ESP_OK;
    }

    size_t copy_len = (size_t)event->data_len;
    size_t remaining = sizeof(buffer->body) - buffer->len - 1;
    if (copy_len > remaining) {
        copy_len = remaining;
    }
    if (copy_len == 0) {
        return ESP_OK;
    }

    memcpy(buffer->body + buffer->len, event->data, copy_len);
    buffer->len += copy_len;
    buffer->body[buffer->len] = '\0';
    return ESP_OK;
}

static bool build_device_server_event_url(char *out_url, size_t out_url_size)
{
    const char *token_server_url = CONFIG_LK_EXAMPLE_TOKEN_SERVER_URL;
    const char *scheme_sep = NULL;
    const char *path_sep = NULL;
    size_t base_len = 0;
    int written = 0;

    if (out_url == NULL || out_url_size == 0 || token_server_url[0] == '\0') {
        return false;
    }

    scheme_sep = strstr(token_server_url, "://");
    if (scheme_sep == NULL) {
        return false;
    }

    path_sep = strchr(scheme_sep + 3, '/');
    base_len = path_sep != NULL ? (size_t)(path_sep - token_server_url) : strlen(token_server_url);
    written = snprintf(out_url, out_url_size, "%.*s/v1/diagnostics/events", (int)base_len, token_server_url);
    return written > 0 && (size_t)written < out_url_size;
}

static char *build_boot_event_payload(const app_diagnostics_snapshot_t *snapshot)
{
    cJSON *root = NULL;
    cJSON *events = NULL;
    cJSON *event = NULL;
    char *body = NULL;

    if (snapshot == NULL) {
        return NULL;
    }

    root = cJSON_CreateObject();
    events = cJSON_CreateArray();
    event = cJSON_CreateObject();
    if (root == NULL || events == NULL || event == NULL) {
        goto cleanup;
    }

    cJSON_AddStringToObject(root, "device_id", active_participant_identity());
    cJSON_AddStringToObject(root, "participant_identity", active_participant_identity());
    cJSON_AddStringToObject(root, "participant_name", CONFIG_LK_EXAMPLE_PARTICIPANT_NAME);
    cJSON_AddStringToObject(root, "device_model", "lichuang_esp32s3");

    cJSON_AddStringToObject(event, "type", "boot_summary");
    cJSON_AddNumberToObject(event, "session_id", snapshot->session_id);
    cJSON_AddNumberToObject(event, "boot_count", snapshot->boot_count);
    cJSON_AddNumberToObject(event, "reboot_streak", snapshot->reboot_streak);
    cJSON_AddStringToObject(event, "reset_reason", app_diagnostics_reset_reason_str(snapshot->reset_reason));
    cJSON_AddBoolToObject(event, "previous_boot_completed", snapshot->previous_boot_completed);
    cJSON_AddStringToObject(event, "previous_breadcrumb", snapshot->previous_breadcrumb);
    cJSON_AddStringToObject(event, "previous_detail", snapshot->previous_detail);
    cJSON_AddStringToObject(event, "current_breadcrumb", snapshot->current_breadcrumb);
    cJSON_AddStringToObject(event, "current_detail", snapshot->current_detail);
    cJSON_AddBoolToObject(event, "coredump_present", snapshot->coredump_present);
    cJSON_AddNumberToObject(event, "coredump_size", snapshot->coredump_size);
    cJSON_AddNumberToObject(event, "coredump_exc_pc", snapshot->coredump_exc_pc);
    cJSON_AddStringToObject(event, "coredump_task", snapshot->coredump_task);
    cJSON_AddStringToObject(event, "coredump_reason", snapshot->coredump_reason);

    cJSON_AddItemToArray(events, event);
    event = NULL;
    cJSON_AddItemToObject(root, "events", events);
    events = NULL;

    body = cJSON_PrintUnformatted(root);

cleanup:
    cJSON_Delete(root);
    cJSON_Delete(events);
    cJSON_Delete(event);
    return body;
}

static void diag_upload_task(void *arg)
{
    diag_upload_task_args_t *task_args = (diag_upload_task_args_t *)arg;
    diag_http_response_buffer_t response = {};
    esp_http_client_handle_t client = NULL;
    char *body = NULL;

    if (task_args == NULL) {
        vTaskDeleteWithCaps(NULL);
        return;
    }

    body = build_boot_event_payload(&task_args->snapshot);
    if (body == NULL) {
        ESP_LOGW(TAG, "Build boot event payload failed");
        goto cleanup;
    }

    esp_http_client_config_t config = {
        .url = task_args->event_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CONFIG_LK_EXAMPLE_TOKEN_SERVER_TIMEOUT_MS,
        .event_handler = diag_http_response_buffer_event_handler,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGW(TAG, "Init diagnostics event HTTP client failed");
        goto cleanup;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "Content-Type", "application/json"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "Accept", "application/json"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_post_field(client, body, (int)strlen(body)));

    ESP_LOGI(TAG, "Uploading boot diagnostics event to %s", task_args->event_url);
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Boot diagnostics upload failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "Boot diagnostics upload returned HTTP %d body=%s", status, response.body);
        goto cleanup;
    }

    ESP_LOGI(TAG, "Boot diagnostics upload accepted: http_status=%d body=%s", status, response.body);

cleanup:
    if (client != NULL) {
        esp_http_client_cleanup(client);
    }
    if (body != NULL) {
        cJSON_free(body);
    }
    free(task_args);
    vTaskDeleteWithCaps(NULL);
}

void app_diagnostics_upload_boot_event_async(void)
{
#if !CONFIG_LK_EXAMPLE_USE_TOKEN_SERVER
    return;
#else
    if (s_boot_event_upload_scheduled) {
        return;
    }

    diag_upload_task_args_t *task_args = (diag_upload_task_args_t *)calloc(1, sizeof(*task_args));
    if (task_args == NULL) {
        ESP_LOGW(TAG, "Allocate diagnostics upload task args failed");
        return;
    }

    if (!build_device_server_event_url(task_args->event_url, sizeof(task_args->event_url))) {
        ESP_LOGW(TAG, "Build diagnostics event URL failed from %s", CONFIG_LK_EXAMPLE_TOKEN_SERVER_URL);
        free(task_args);
        return;
    }

    app_diagnostics_copy_snapshot(&task_args->snapshot);

    BaseType_t task_ok = xTaskCreateWithCaps(
        diag_upload_task,
        "diag_upload",
        DIAG_UPLOAD_TASK_STACK_SIZE,
        task_args,
        DIAG_UPLOAD_TASK_PRIORITY,
        NULL,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (task_ok != pdPASS) {
        ESP_LOGW(TAG, "Start diagnostics upload task failed");
        free(task_args);
        return;
    }

    s_boot_event_upload_scheduled = true;
#endif
}
