#include "app_downlink_capture.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_diagnostics.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#ifndef CONFIG_LK_EXAMPLE_DEBUG_DOWNLINK_HTTP_RINGBUF_KB
#define CONFIG_LK_EXAMPLE_DEBUG_DOWNLINK_HTTP_RINGBUF_KB 32
#endif

#ifndef CONFIG_LK_EXAMPLE_DEBUG_DOWNLINK_HTTP_IDLE_FLUSH_MS
#define CONFIG_LK_EXAMPLE_DEBUG_DOWNLINK_HTTP_IDLE_FLUSH_MS 1500
#endif

#ifndef CONFIG_LK_EXAMPLE_DEBUG_DOWNLINK_HTTP_SEGMENT_SECONDS
#define CONFIG_LK_EXAMPLE_DEBUG_DOWNLINK_HTTP_SEGMENT_SECONDS 20
#endif

#if CONFIG_LK_EXAMPLE_DEBUG_DOWNLINK_HTTP_UPLOAD

static const char *TAG = "downlink_capture";

#define DOWNLINK_CAPTURE_BASE_PATH "/spiffs"
#define DOWNLINK_CAPTURE_PARTITION "storage"
#define DOWNLINK_CAPTURE_KIND "downlink_audio"
#define DOWNLINK_CAPTURE_WAV_HEADER_SIZE 44
#define DOWNLINK_CAPTURE_IO_CHUNK_SIZE 2048
#define DOWNLINK_CAPTURE_RINGBUF_SIZE ((size_t)CONFIG_LK_EXAMPLE_DEBUG_DOWNLINK_HTTP_RINGBUF_KB * 1024U)
#define DOWNLINK_CAPTURE_UPLOAD_QUEUE_LEN 4
#define DOWNLINK_CAPTURE_WRITER_TASK_STACK_SIZE (6 * 1024)
#define DOWNLINK_CAPTURE_UPLOADER_TASK_STACK_SIZE (8 * 1024)
#define DOWNLINK_CAPTURE_TASK_PRIORITY 4
#define DOWNLINK_CAPTURE_RESPONSE_MAX 512
#define DOWNLINK_CAPTURE_FILE_PATH_MAX 160
#define DOWNLINK_CAPTURE_FILE_NAME_MAX 96
#define DOWNLINK_CAPTURE_URL_MAX 256
#define DOWNLINK_CAPTURE_BYTES_PER_SECOND (16000U * 2U)
#define DOWNLINK_CAPTURE_MAX_PCM_BYTES ((size_t)CONFIG_LK_EXAMPLE_DEBUG_DOWNLINK_HTTP_SEGMENT_SECONDS * DOWNLINK_CAPTURE_BYTES_PER_SECOND)

typedef struct {
    char body[DOWNLINK_CAPTURE_RESPONSE_MAX];
    size_t len;
} downlink_capture_http_response_t;

typedef struct {
    char file_path[DOWNLINK_CAPTURE_FILE_PATH_MAX];
    char filename[DOWNLINK_CAPTURE_FILE_NAME_MAX];
    uint32_t session_id;
    uint32_t boot_count;
    uint32_t reboot_streak;
    uint32_t segment_index;
    size_t pcm_bytes;
} downlink_capture_upload_job_t;

typedef struct {
    bool initialized;
    bool spiffs_mounted;
    RingbufHandle_t ringbuf;
    QueueHandle_t upload_queue;
    TaskHandle_t writer_task;
    TaskHandle_t uploader_task;
    SemaphoreHandle_t lock;
    FILE *file;
    size_t pcm_bytes;
    uint32_t segment_index;
    uint32_t dropped_frames;
    app_diagnostics_snapshot_t segment_snapshot;
    char current_file_path[DOWNLINK_CAPTURE_FILE_PATH_MAX];
    char current_filename[DOWNLINK_CAPTURE_FILE_NAME_MAX];
} downlink_capture_state_t;

static downlink_capture_state_t s_downlink_capture;

static BaseType_t downlink_capture_create_task(
    TaskFunction_t task_fn,
    const char *task_name,
    uint32_t stack_size,
    void *task_arg,
    UBaseType_t priority,
    TaskHandle_t *task_handle)
{
    // This path performs SPIFFS/flash I/O, so task stacks must stay in internal RAM.
    BaseType_t ret = xTaskCreateWithCaps(
        task_fn,
        task_name,
        stack_size,
        task_arg,
        priority,
        task_handle,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ret == pdPASS) {
        return ret;
    }

    ESP_LOGW(TAG, "Create task %s with internal RAM stack failed, retrying with default allocator", task_name);
    return xTaskCreate(task_fn, task_name, stack_size, task_arg, priority, task_handle);
}

static const char *active_participant_identity(void)
{
    return strlen(CONFIG_LK_EXAMPLE_PARTICIPANT_IDENTITY) > 0
        ? CONFIG_LK_EXAMPLE_PARTICIPANT_IDENTITY
        : CONFIG_LK_EXAMPLE_PARTICIPANT_NAME;
}

static void write_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void write_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
    dst[2] = (uint8_t)((value >> 16) & 0xFFU);
    dst[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static esp_err_t write_wav_header(FILE *fp, size_t pcm_bytes)
{
    if (fp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t header[DOWNLINK_CAPTURE_WAV_HEADER_SIZE] = {0};
    const uint32_t byte_rate = 16000U * 1U * 2U;
    const uint16_t block_align = 2U;
    const uint32_t data_size = (uint32_t)pcm_bytes;
    const uint32_t riff_size = data_size + 36U;

    memcpy(header + 0, "RIFF", 4);
    write_le32(header + 4, riff_size);
    memcpy(header + 8, "WAVE", 4);
    memcpy(header + 12, "fmt ", 4);
    write_le32(header + 16, 16U);
    write_le16(header + 20, 1U);
    write_le16(header + 22, 1U);
    write_le32(header + 24, 16000U);
    write_le32(header + 28, byte_rate);
    write_le16(header + 32, block_align);
    write_le16(header + 34, 16U);
    memcpy(header + 36, "data", 4);
    write_le32(header + 40, data_size);

    if (fseek(fp, 0, SEEK_SET) != 0) {
        return ESP_FAIL;
    }
    if (fwrite(header, 1, sizeof(header), fp) != sizeof(header)) {
        return ESP_FAIL;
    }
    if (fflush(fp) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t mount_spiffs_if_needed(void)
{
    if (esp_spiffs_mounted(DOWNLINK_CAPTURE_PARTITION)) {
        s_downlink_capture.spiffs_mounted = true;
    } else {
        esp_vfs_spiffs_conf_t conf = {
            .base_path = DOWNLINK_CAPTURE_BASE_PATH,
            .partition_label = DOWNLINK_CAPTURE_PARTITION,
            .max_files = 8,
            .format_if_mount_failed = true,
        };
        ESP_RETURN_ON_ERROR(esp_vfs_spiffs_register(&conf), TAG, "mount SPIFFS failed");
        s_downlink_capture.spiffs_mounted = true;
    }

    size_t total = 0;
    size_t used = 0;
    ESP_RETURN_ON_ERROR(esp_spiffs_info(DOWNLINK_CAPTURE_PARTITION, &total, &used), TAG, "read SPIFFS info failed");
    ESP_LOGI(TAG, "Downlink capture storage ready: total=%u used=%u free=%u",
        (unsigned)total,
        (unsigned)used,
        (unsigned)(total - used));
    return ESP_OK;
}

static bool build_blob_url(char *out_url, size_t out_url_size)
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
    written = snprintf(out_url, out_url_size, "%.*s/v1/diagnostics/blobs", (int)base_len, token_server_url);
    return written > 0 && (size_t)written < out_url_size;
}

static esp_err_t response_event_handler(esp_http_client_event_t *event)
{
    downlink_capture_http_response_t *buffer = (downlink_capture_http_response_t *)event->user_data;
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

static esp_err_t start_segment_locked(void)
{
    if (s_downlink_capture.file != NULL) {
        return ESP_OK;
    }

    s_downlink_capture.segment_index += 1U;
    s_downlink_capture.pcm_bytes = 0;
    memset(&s_downlink_capture.segment_snapshot, 0, sizeof(s_downlink_capture.segment_snapshot));
    app_diagnostics_copy_snapshot(&s_downlink_capture.segment_snapshot);

    int name_len = snprintf(
        s_downlink_capture.current_filename,
        sizeof(s_downlink_capture.current_filename),
        "downlink_s%" PRIu32 "_b%" PRIu32 "_seg%03" PRIu32 ".wav",
        s_downlink_capture.segment_snapshot.session_id,
        s_downlink_capture.segment_snapshot.boot_count,
        s_downlink_capture.segment_index);
    if (name_len <= 0 || name_len >= (int)sizeof(s_downlink_capture.current_filename)) {
        return ESP_ERR_INVALID_SIZE;
    }

    int path_len = snprintf(
        s_downlink_capture.current_file_path,
        sizeof(s_downlink_capture.current_file_path),
        "%s/%s",
        DOWNLINK_CAPTURE_BASE_PATH,
        s_downlink_capture.current_filename);
    if (path_len <= 0 || path_len >= (int)sizeof(s_downlink_capture.current_file_path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    s_downlink_capture.file = fopen(s_downlink_capture.current_file_path, "wb+");
    if (s_downlink_capture.file == NULL) {
        s_downlink_capture.current_file_path[0] = '\0';
        s_downlink_capture.current_filename[0] = '\0';
        return ESP_FAIL;
    }
    if (write_wav_header(s_downlink_capture.file, 0) != ESP_OK) {
        fclose(s_downlink_capture.file);
        s_downlink_capture.file = NULL;
        remove(s_downlink_capture.current_file_path);
        s_downlink_capture.current_file_path[0] = '\0';
        s_downlink_capture.current_filename[0] = '\0';
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
        "Start downlink segment session=%" PRIu32 " boot=%" PRIu32 " segment=%" PRIu32 " file=%s",
        s_downlink_capture.segment_snapshot.session_id,
        s_downlink_capture.segment_snapshot.boot_count,
        s_downlink_capture.segment_index,
        s_downlink_capture.current_filename);
    return ESP_OK;
}

static bool finalize_segment_locked(downlink_capture_upload_job_t *out_job)
{
    if (s_downlink_capture.file == NULL) {
        return false;
    }

    FILE *file = s_downlink_capture.file;
    s_downlink_capture.file = NULL;

    if (write_wav_header(file, s_downlink_capture.pcm_bytes) != ESP_OK) {
        ESP_LOGW(TAG, "Finalize WAV header failed for %s", s_downlink_capture.current_filename);
    }
    fclose(file);

    if (s_downlink_capture.pcm_bytes == 0) {
        remove(s_downlink_capture.current_file_path);
        s_downlink_capture.current_file_path[0] = '\0';
        s_downlink_capture.current_filename[0] = '\0';
        return false;
    }

    if (out_job != NULL) {
        memset(out_job, 0, sizeof(*out_job));
        strlcpy(out_job->file_path, s_downlink_capture.current_file_path, sizeof(out_job->file_path));
        strlcpy(out_job->filename, s_downlink_capture.current_filename, sizeof(out_job->filename));
        out_job->session_id = s_downlink_capture.segment_snapshot.session_id;
        out_job->boot_count = s_downlink_capture.segment_snapshot.boot_count;
        out_job->reboot_streak = s_downlink_capture.segment_snapshot.reboot_streak;
        out_job->segment_index = s_downlink_capture.segment_index;
        out_job->pcm_bytes = s_downlink_capture.pcm_bytes;
    }

    ESP_LOGI(TAG,
        "Finalize downlink segment session=%" PRIu32 " boot=%" PRIu32
        " segment=%" PRIu32 " pcm_bytes=%u",
        s_downlink_capture.segment_snapshot.session_id,
        s_downlink_capture.segment_snapshot.boot_count,
        s_downlink_capture.segment_index,
        (unsigned)s_downlink_capture.pcm_bytes);

    s_downlink_capture.current_file_path[0] = '\0';
    s_downlink_capture.current_filename[0] = '\0';
    s_downlink_capture.pcm_bytes = 0;
    return true;
}

static void enqueue_upload_job(const downlink_capture_upload_job_t *job)
{
    if (job == NULL || job->file_path[0] == '\0' || s_downlink_capture.upload_queue == NULL) {
        return;
    }

    if (xQueueSend(s_downlink_capture.upload_queue, job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Downlink upload queue full, preserving file %s", job->file_path);
        return;
    }
}

static esp_err_t upload_file_job(const downlink_capture_upload_job_t *job)
{
    if (job == NULL || job->file_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char url[DOWNLINK_CAPTURE_URL_MAX];
    if (!build_blob_url(url, sizeof(url))) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *fp = fopen(job->file_path, "rb");
    if (fp == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    size_t file_size = job->pcm_bytes + DOWNLINK_CAPTURE_WAV_HEADER_SIZE;
    downlink_capture_http_response_t response = {};
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CONFIG_LK_EXAMPLE_TOKEN_SERVER_TIMEOUT_MS,
        .event_handler = response_event_handler,
        .user_data = &response,
    };
    if (strncmp(url, "https://", 8) == 0) {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "Content-Type", "audio/wav"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "Accept", "application/json"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "X-Device-Id", active_participant_identity()));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "X-Diag-Kind", DOWNLINK_CAPTURE_KIND));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "X-File-Name", job->filename));

    char value[32];
    snprintf(value, sizeof(value), "%" PRIu32, job->session_id);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "X-Session-Id", value));
    snprintf(value, sizeof(value), "%" PRIu32, job->boot_count);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "X-Boot-Count", value));
    snprintf(value, sizeof(value), "%" PRIu32, job->reboot_streak);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "X-Reboot-Streak", value));

    esp_err_t err = esp_http_client_open(client, (int)file_size);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        fclose(fp);
        return err;
    }

    char buffer[DOWNLINK_CAPTURE_IO_CHUNK_SIZE];
    while (!feof(fp)) {
        size_t read_bytes = fread(buffer, 1, sizeof(buffer), fp);
        if (read_bytes == 0) {
            break;
        }
        int written = esp_http_client_write(client, buffer, (int)read_bytes);
        if (written < 0 || written != (int)read_bytes) {
            err = ESP_FAIL;
            break;
        }
    }
    fclose(fp);

    if (err == ESP_OK) {
        if (esp_http_client_fetch_headers(client) < 0) {
            err = ESP_FAIL;
        }
    }

    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status < 200 || status >= 300) {
            err = ESP_FAIL;
            ESP_LOGW(TAG, "Downlink upload returned HTTP %d body=%s", status, response.body);
        } else {
            ESP_LOGI(TAG,
                "Downlink upload accepted: session=%" PRIu32 " segment=%" PRIu32
                " http_status=%d body=%s",
                job->session_id,
                job->segment_index,
                status,
                response.body);
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

static void uploader_task(void *arg)
{
    (void)arg;

    downlink_capture_upload_job_t job = {};
    while (true) {
        if (xQueueReceive(s_downlink_capture.upload_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        esp_err_t err = upload_file_job(&job);
        if (err == ESP_OK) {
            remove(job.file_path);
        } else {
            ESP_LOGW(TAG,
                "Downlink upload failed for %s: %s (staged file preserved)",
                job.file_path,
                esp_err_to_name(err));
        }
    }
}

static void writer_task(void *arg)
{
    (void)arg;

    while (true) {
        size_t item_size = 0;
        uint8_t *item = (uint8_t *)xRingbufferReceive(
            s_downlink_capture.ringbuf,
            &item_size,
            pdMS_TO_TICKS(CONFIG_LK_EXAMPLE_DEBUG_DOWNLINK_HTTP_IDLE_FLUSH_MS));

        downlink_capture_upload_job_t job = {};
        bool have_job = false;

        if (item != NULL) {
            if (xSemaphoreTake(s_downlink_capture.lock, portMAX_DELAY) == pdTRUE) {
                if (start_segment_locked() == ESP_OK && s_downlink_capture.file != NULL) {
                    size_t written = fwrite(item, 1, item_size, s_downlink_capture.file);
                    if (written == item_size) {
                        s_downlink_capture.pcm_bytes += written;
                        if (s_downlink_capture.pcm_bytes >= DOWNLINK_CAPTURE_MAX_PCM_BYTES) {
                            have_job = finalize_segment_locked(&job);
                        }
                    } else {
                        ESP_LOGW(TAG, "Write downlink PCM failed: want=%u got=%u",
                            (unsigned)item_size,
                            (unsigned)written);
                        have_job = finalize_segment_locked(&job);
                    }
                }
                xSemaphoreGive(s_downlink_capture.lock);
            }
            vRingbufferReturnItem(s_downlink_capture.ringbuf, item);
        } else {
            if (xSemaphoreTake(s_downlink_capture.lock, portMAX_DELAY) == pdTRUE) {
                have_job = finalize_segment_locked(&job);
                xSemaphoreGive(s_downlink_capture.lock);
            }
        }

        if (have_job) {
            enqueue_upload_job(&job);
        }
    }
}

esp_err_t app_downlink_capture_init(void)
{
    if (s_downlink_capture.initialized) {
        return ESP_OK;
    }
    if (CONFIG_LK_EXAMPLE_TOKEN_SERVER_URL[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_downlink_capture, 0, sizeof(s_downlink_capture));
    s_downlink_capture.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_downlink_capture.lock != NULL, ESP_ERR_NO_MEM, TAG, "create mutex failed");

    ESP_RETURN_ON_ERROR(mount_spiffs_if_needed(), TAG, "mount capture storage failed");

    s_downlink_capture.ringbuf = xRingbufferCreateWithCaps(
        DOWNLINK_CAPTURE_RINGBUF_SIZE,
        RINGBUF_TYPE_BYTEBUF,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_downlink_capture.ringbuf != NULL, ESP_ERR_NO_MEM, TAG, "create ring buffer failed");

    s_downlink_capture.upload_queue = xQueueCreateWithCaps(
        DOWNLINK_CAPTURE_UPLOAD_QUEUE_LEN,
        sizeof(downlink_capture_upload_job_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_downlink_capture.upload_queue != NULL, ESP_ERR_NO_MEM, TAG, "create upload queue failed");

    if (downlink_capture_create_task(
            writer_task,
            "dbg_downlink_cap",
            DOWNLINK_CAPTURE_WRITER_TASK_STACK_SIZE,
            NULL,
            DOWNLINK_CAPTURE_TASK_PRIORITY,
            &s_downlink_capture.writer_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (downlink_capture_create_task(
            uploader_task,
            "dbg_downlink_up",
            DOWNLINK_CAPTURE_UPLOADER_TASK_STACK_SIZE,
            NULL,
            DOWNLINK_CAPTURE_TASK_PRIORITY,
            &s_downlink_capture.uploader_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_downlink_capture.initialized = true;
    ESP_LOGI(TAG,
        "Downlink HTTP capture enabled: ringbuf=%uKB idle_flush_ms=%u max_segment_seconds=%u",
        (unsigned)CONFIG_LK_EXAMPLE_DEBUG_DOWNLINK_HTTP_RINGBUF_KB,
        (unsigned)CONFIG_LK_EXAMPLE_DEBUG_DOWNLINK_HTTP_IDLE_FLUSH_MS,
        (unsigned)CONFIG_LK_EXAMPLE_DEBUG_DOWNLINK_HTTP_SEGMENT_SECONDS);
    return ESP_OK;
}

void app_downlink_capture_on_pcm(const uint8_t *data, size_t size)
{
    if (!s_downlink_capture.initialized || s_downlink_capture.ringbuf == NULL || data == NULL || size == 0) {
        return;
    }

    if (xRingbufferSend(s_downlink_capture.ringbuf, data, size, 0) != pdTRUE) {
        s_downlink_capture.dropped_frames += 1U;
        if ((s_downlink_capture.dropped_frames % 100U) == 1U) {
            ESP_LOGW(TAG, "Downlink capture dropped frames=%u", s_downlink_capture.dropped_frames);
        }
    }
}

#else

esp_err_t app_downlink_capture_init(void)
{
    return ESP_OK;
}

void app_downlink_capture_on_pcm(const uint8_t *data, size_t size)
{
    (void)data;
    (void)size;
}

#endif
