#include "livekit_debug_audio.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_capture_sink.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "livekit_media.h"
#include "sdkconfig.h"

#ifndef CONFIG_LK_EXAMPLE_DEBUG_DOWNLINK_RINGBUF_KB
#define CONFIG_LK_EXAMPLE_DEBUG_DOWNLINK_RINGBUF_KB 24
#endif

#if CONFIG_LK_EXAMPLE_DEBUG_UPLINK_WAV
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#endif

#if CONFIG_LK_EXAMPLE_DEBUG_UPLINK_WS || CONFIG_LK_EXAMPLE_DEBUG_DOWNLINK_WS
#include "esp_event.h"
#include "esp_websocket_client.h"
#include "freertos/ringbuf.h"
#endif

static const char *TAG = "lk_debug_audio";

#define DEBUG_AUDIO_SAMPLE_RATE 16000
#define DEBUG_AUDIO_CHANNELS 1
#define DEBUG_AUDIO_BITS_PER_SAMPLE 16

#define RETURN_ON_CAPTURE_ERROR(expr, message) do {                     \
    esp_capture_err_t __capture_ret = (expr);                           \
    if (__capture_ret != ESP_CAPTURE_ERR_OK) {                          \
        ESP_LOGE(TAG, "%s: %d", message, __capture_ret);                \
        return ESP_FAIL;                                                \
    }                                                                   \
} while (0)

#if CONFIG_LK_EXAMPLE_DEBUG_UPLINK_WAV

#define DEBUG_AUDIO_BASE_PATH "/spiffs"
#define DEBUG_AUDIO_PARTITION "storage"
#define DEBUG_AUDIO_FILE_PATH DEBUG_AUDIO_BASE_PATH "/uplink_debug.wav"
#define DEBUG_AUDIO_CHUNK_SIZE 2048
#define DEBUG_AUDIO_WAV_HEADER_SIZE 44

typedef struct {
    bool initialized;
    bool spiffs_mounted;
    bool recording;
    bool file_ready;
    size_t file_size;
    size_t pcm_bytes;
    httpd_handle_t httpd;
    esp_capture_sink_handle_t sink;
    esp_timer_handle_t stop_timer;
    TaskHandle_t writer_task;
    FILE *file;
    SemaphoreHandle_t lock;
} debug_audio_state_t;

static debug_audio_state_t s_debug_audio;

static void debug_audio_log_urls(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        ESP_LOGI(TAG, "Debug audio server ready on port %d", CONFIG_LK_EXAMPLE_DEBUG_HTTP_PORT);
        return;
    }

    esp_netif_ip_info_t ip_info = {};
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        ESP_LOGI(TAG, "Debug audio server ready on port %d", CONFIG_LK_EXAMPLE_DEBUG_HTTP_PORT);
        return;
    }

    ESP_LOGI(TAG,
        "Debug audio URLs:\n"
        "  start : http://" IPSTR ":%d/debug/start\n"
        "  status: http://" IPSTR ":%d/debug/status\n"
        "  wav   : http://" IPSTR ":%d/debug/uplink.wav",
        IP2STR(&ip_info.ip),
        CONFIG_LK_EXAMPLE_DEBUG_HTTP_PORT,
        IP2STR(&ip_info.ip),
        CONFIG_LK_EXAMPLE_DEBUG_HTTP_PORT,
        IP2STR(&ip_info.ip),
        CONFIG_LK_EXAMPLE_DEBUG_HTTP_PORT);
}

static void debug_audio_refresh_file_state_locked(void)
{
    struct stat st = {};
    if (stat(DEBUG_AUDIO_FILE_PATH, &st) == 0 && st.st_size > DEBUG_AUDIO_WAV_HEADER_SIZE) {
        s_debug_audio.file_ready = true;
        s_debug_audio.file_size = (size_t)st.st_size;
    } else {
        s_debug_audio.file_ready = false;
        s_debug_audio.file_size = 0;
    }
}

static void debug_audio_write_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void debug_audio_write_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
    dst[2] = (uint8_t)((value >> 16) & 0xFFU);
    dst[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static esp_err_t debug_audio_write_wav_header(FILE *fp, size_t pcm_bytes)
{
    if (fp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t header[DEBUG_AUDIO_WAV_HEADER_SIZE] = {0};
    const uint32_t byte_rate = DEBUG_AUDIO_SAMPLE_RATE * DEBUG_AUDIO_CHANNELS * (DEBUG_AUDIO_BITS_PER_SAMPLE / 8U);
    const uint16_t block_align = (uint16_t)(DEBUG_AUDIO_CHANNELS * (DEBUG_AUDIO_BITS_PER_SAMPLE / 8U));
    const uint32_t data_size = (uint32_t)pcm_bytes;
    const uint32_t riff_size = data_size + 36U;

    memcpy(header + 0, "RIFF", 4);
    debug_audio_write_le32(header + 4, riff_size);
    memcpy(header + 8, "WAVE", 4);
    memcpy(header + 12, "fmt ", 4);
    debug_audio_write_le32(header + 16, 16);
    debug_audio_write_le16(header + 20, 1);
    debug_audio_write_le16(header + 22, DEBUG_AUDIO_CHANNELS);
    debug_audio_write_le32(header + 24, DEBUG_AUDIO_SAMPLE_RATE);
    debug_audio_write_le32(header + 28, byte_rate);
    debug_audio_write_le16(header + 32, block_align);
    debug_audio_write_le16(header + 34, DEBUG_AUDIO_BITS_PER_SAMPLE);
    memcpy(header + 36, "data", 4);
    debug_audio_write_le32(header + 40, data_size);

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

static void debug_audio_finalize_file_locked(void)
{
    if (s_debug_audio.file == NULL) {
        debug_audio_refresh_file_state_locked();
        return;
    }

    if (debug_audio_write_wav_header(s_debug_audio.file, s_debug_audio.pcm_bytes) != ESP_OK) {
        ESP_LOGE(TAG, "Finalize WAV header failed");
    }
    fclose(s_debug_audio.file);
    s_debug_audio.file = NULL;
    debug_audio_refresh_file_state_locked();
}

static void debug_audio_writer_task(void *arg)
{
    (void)arg;

    esp_capture_stream_frame_t frame = {
        .stream_type = ESP_CAPTURE_STREAM_TYPE_AUDIO,
    };

    while (true) {
        esp_capture_err_t ret = esp_capture_sink_acquire_frame(s_debug_audio.sink, &frame, false);

        bool recording = false;
        if (xSemaphoreTake(s_debug_audio.lock, portMAX_DELAY) == pdTRUE) {
            recording = s_debug_audio.recording;
            xSemaphoreGive(s_debug_audio.lock);
        }

        if (ret == ESP_CAPTURE_ERR_OK) {
            if (xSemaphoreTake(s_debug_audio.lock, portMAX_DELAY) == pdTRUE) {
                if (s_debug_audio.file != NULL && frame.data != NULL && frame.size > 0) {
                    size_t written = fwrite(frame.data, 1, (size_t)frame.size, s_debug_audio.file);
                    if (written == (size_t)frame.size) {
                        s_debug_audio.pcm_bytes += written;
                    } else {
                        ESP_LOGE(TAG, "Write PCM frame failed: want=%d got=%u", frame.size, (unsigned)written);
                    }
                }
                xSemaphoreGive(s_debug_audio.lock);
            }
            esp_capture_sink_release_frame(s_debug_audio.sink, &frame);
            continue;
        }

        if (recording) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        break;
    }

    if (xSemaphoreTake(s_debug_audio.lock, portMAX_DELAY) == pdTRUE) {
        debug_audio_finalize_file_locked();
        s_debug_audio.writer_task = NULL;
        ESP_LOGI(TAG,
            "Processed uplink WAV ready=%d size=%u pcm_bytes=%u path=%s",
            s_debug_audio.file_ready,
            (unsigned)s_debug_audio.file_size,
            (unsigned)s_debug_audio.pcm_bytes,
            DEBUG_AUDIO_FILE_PATH);
        xSemaphoreGive(s_debug_audio.lock);
    }
    vTaskDelete(NULL);
}

static void debug_audio_stop_recording_task(void *arg)
{
    (void)arg;

    if (xSemaphoreTake(s_debug_audio.lock, portMAX_DELAY) == pdTRUE) {
        s_debug_audio.recording = false;
        debug_audio_finalize_file_locked();
        xSemaphoreGive(s_debug_audio.lock);
    }

    if (s_debug_audio.sink != NULL) {
        esp_capture_err_t ret = esp_capture_sink_enable(s_debug_audio.sink, ESP_CAPTURE_RUN_MODE_DISABLE);
        if (ret != ESP_CAPTURE_ERR_OK) {
            ESP_LOGW(TAG, "Disable debug audio sink failed: %d", ret);
        }
    }
    vTaskDelete(NULL);
}

static void debug_audio_stop_timer_cb(void *arg)
{
    (void)arg;
    if (xTaskCreate(debug_audio_stop_recording_task, "dbg_uplink_stop", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create debug stop task");
    }
}

esp_err_t livekit_debug_audio_start_recording(void)
{
    ESP_RETURN_ON_FALSE(s_debug_audio.sink != NULL, ESP_ERR_INVALID_STATE, TAG, "debug sink not ready");

    if (xSemaphoreTake(s_debug_audio.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_debug_audio.recording) {
        xSemaphoreGive(s_debug_audio.lock);
        return ESP_ERR_INVALID_STATE;
    }

    remove(DEBUG_AUDIO_FILE_PATH);
    s_debug_audio.recording = false;
    s_debug_audio.file_ready = false;
    s_debug_audio.file_size = 0;
    s_debug_audio.pcm_bytes = 0;

    s_debug_audio.file = fopen(DEBUG_AUDIO_FILE_PATH, "wb+");
    if (s_debug_audio.file == NULL) {
        xSemaphoreGive(s_debug_audio.lock);
        ESP_LOGE(TAG, "Open debug WAV failed: %s", DEBUG_AUDIO_FILE_PATH);
        return ESP_FAIL;
    }
    if (debug_audio_write_wav_header(s_debug_audio.file, 0) != ESP_OK) {
        fclose(s_debug_audio.file);
        s_debug_audio.file = NULL;
        xSemaphoreGive(s_debug_audio.lock);
        ESP_LOGE(TAG, "Write initial WAV header failed");
        return ESP_FAIL;
    }

    if (s_debug_audio.writer_task == NULL) {
        if (xTaskCreate(debug_audio_writer_task, "dbg_uplink_writer", 6144, NULL, 4, &s_debug_audio.writer_task) != pdPASS) {
            fclose(s_debug_audio.file);
            s_debug_audio.file = NULL;
            xSemaphoreGive(s_debug_audio.lock);
            ESP_LOGE(TAG, "Create debug writer task failed");
            return ESP_ERR_NO_MEM;
        }
    }

    s_debug_audio.recording = true;
    xSemaphoreGive(s_debug_audio.lock);

    RETURN_ON_CAPTURE_ERROR(
        esp_capture_sink_enable(s_debug_audio.sink, ESP_CAPTURE_RUN_MODE_ALWAYS),
        "Enable debug audio sink failed");

    esp_err_t timer_ret = esp_timer_stop(s_debug_audio.stop_timer);
    if (timer_ret != ESP_OK && timer_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Stopping prior debug timer failed: %s", esp_err_to_name(timer_ret));
    }
    ESP_RETURN_ON_ERROR(
        esp_timer_start_once(s_debug_audio.stop_timer, (uint64_t)CONFIG_LK_EXAMPLE_DEBUG_UPLINK_RECORD_SECONDS * 1000000ULL),
        TAG,
        "start stop timer failed");

    ESP_LOGI(TAG, "Started processed uplink WAV capture for %d seconds", CONFIG_LK_EXAMPLE_DEBUG_UPLINK_RECORD_SECONDS);
    debug_audio_log_urls();
    return ESP_OK;
}

static esp_err_t debug_audio_mount_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = DEBUG_AUDIO_BASE_PATH,
        .partition_label = DEBUG_AUDIO_PARTITION,
        .max_files = 6,
        .format_if_mount_failed = true,
    };
    ESP_RETURN_ON_ERROR(esp_vfs_spiffs_register(&conf), TAG, "mount SPIFFS failed");

    size_t total = 0;
    size_t used = 0;
    ESP_RETURN_ON_ERROR(esp_spiffs_info(DEBUG_AUDIO_PARTITION, &total, &used), TAG, "read SPIFFS info failed");
    s_debug_audio.spiffs_mounted = true;
    ESP_LOGI(TAG, "SPIFFS mounted: total=%u used=%u free=%u", (unsigned)total, (unsigned)used, (unsigned)(total - used));
    return ESP_OK;
}

static esp_err_t debug_audio_setup_sink(void)
{
    esp_capture_handle_t capturer = livekit_media_get_capturer();
    ESP_RETURN_ON_FALSE(capturer != NULL, ESP_ERR_INVALID_STATE, TAG, "capturer not ready");

    esp_capture_sink_cfg_t sink_cfg = {
        .audio_info = {
            .format_id = ESP_CAPTURE_FMT_ID_PCM,
            .sample_rate = DEBUG_AUDIO_SAMPLE_RATE,
            .channel = DEBUG_AUDIO_CHANNELS,
            .bits_per_sample = DEBUG_AUDIO_BITS_PER_SAMPLE,
        },
    };
    RETURN_ON_CAPTURE_ERROR(
        esp_capture_sink_setup(capturer, 1, &sink_cfg, &s_debug_audio.sink),
        "setup debug sink failed");
    RETURN_ON_CAPTURE_ERROR(
        esp_capture_sink_enable(s_debug_audio.sink, ESP_CAPTURE_RUN_MODE_DISABLE),
        "disable debug sink by default failed");

    ESP_LOGI(TAG, "Processed uplink debug sink configured: PCM 16k mono -> %s", DEBUG_AUDIO_FILE_PATH);
    return ESP_OK;
}

static esp_err_t debug_audio_status_handler(httpd_req_t *req)
{
    char body[256];
    bool recording = false;
    bool file_ready = false;
    size_t file_size = 0;

    if (xSemaphoreTake(s_debug_audio.lock, portMAX_DELAY) == pdTRUE) {
        debug_audio_refresh_file_state_locked();
        recording = s_debug_audio.recording;
        file_ready = s_debug_audio.file_ready;
        file_size = s_debug_audio.file_size;
        if (recording) {
            file_ready = false;
        }
        xSemaphoreGive(s_debug_audio.lock);
    }

    int len = snprintf(
        body,
        sizeof(body),
        "{\"recording\":%s,\"file_ready\":%s,\"file_size\":%u,\"duration_seconds\":%d,"
        "\"download\":\"/debug/uplink.wav\",\"start\":\"/debug/start\"}\n",
        recording ? "true" : "false",
        file_ready ? "true" : "false",
        (unsigned)file_size,
        CONFIG_LK_EXAMPLE_DEBUG_UPLINK_RECORD_SECONDS);
    if (len < 0) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t debug_audio_start_handler(httpd_req_t *req)
{
    esp_err_t ret = livekit_debug_audio_start_recording();
    if (ret == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "recording already in progress\n");
    }
    if (ret != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "failed to start debug recording\n");
    }

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "started processed uplink WAV capture\n");
}

static esp_err_t debug_audio_wav_handler(httpd_req_t *req)
{
    bool recording = false;
    bool file_ready = false;

    if (xSemaphoreTake(s_debug_audio.lock, portMAX_DELAY) == pdTRUE) {
        debug_audio_refresh_file_state_locked();
        recording = s_debug_audio.recording;
        file_ready = s_debug_audio.file_ready;
        xSemaphoreGive(s_debug_audio.lock);
    }

    if (recording) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "recording in progress, try again after it finishes\n");
    }
    if (!file_ready) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "no processed uplink WAV available yet\n");
    }

    FILE *fp = fopen(DEBUG_AUDIO_FILE_PATH, "rb");
    if (fp == NULL) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "failed to open WAV file\n");
    }

    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"uplink_debug.wav\"");

    char buffer[DEBUG_AUDIO_CHUNK_SIZE];
    size_t read_bytes = 0;
    esp_err_t ret = ESP_OK;
    while ((read_bytes = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        if (httpd_resp_send_chunk(req, buffer, read_bytes) != ESP_OK) {
            ret = ESP_FAIL;
            break;
        }
    }
    fclose(fp);

    if (ret == ESP_OK) {
        ret = httpd_resp_send_chunk(req, NULL, 0);
    }
    return ret;
}

static esp_err_t debug_audio_start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_LK_EXAMPLE_DEBUG_HTTP_PORT;
    config.ctrl_port = CONFIG_LK_EXAMPLE_DEBUG_HTTP_PORT + 1;
    config.max_uri_handlers = 8;
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_RETURN_ON_ERROR(httpd_start(&s_debug_audio.httpd, &config), TAG, "start debug http server failed");

    const httpd_uri_t status_uri = {
        .uri = "/debug/status",
        .method = HTTP_GET,
        .handler = debug_audio_status_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t start_uri = {
        .uri = "/debug/start",
        .method = HTTP_GET,
        .handler = debug_audio_start_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t wav_uri = {
        .uri = "/debug/uplink.wav",
        .method = HTTP_GET,
        .handler = debug_audio_wav_handler,
        .user_ctx = NULL,
    };

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_debug_audio.httpd, &status_uri), TAG, "register status uri failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_debug_audio.httpd, &start_uri), TAG, "register start uri failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_debug_audio.httpd, &wav_uri), TAG, "register wav uri failed");
    return ESP_OK;
}

esp_err_t livekit_debug_audio_init(void)
{
    if (s_debug_audio.initialized) {
        return ESP_OK;
    }

    memset(&s_debug_audio, 0, sizeof(s_debug_audio));
    s_debug_audio.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_debug_audio.lock != NULL, ESP_ERR_NO_MEM, TAG, "create debug mutex failed");

    ESP_RETURN_ON_ERROR(debug_audio_mount_spiffs(), TAG, "mount debug storage failed");
    ESP_RETURN_ON_ERROR(debug_audio_setup_sink(), TAG, "setup debug sink failed");

    esp_timer_create_args_t timer_args = {
        .callback = debug_audio_stop_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "dbg_uplink_stop",
        .skip_unhandled_events = true,
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_debug_audio.stop_timer), TAG, "create debug timer failed");
    ESP_RETURN_ON_ERROR(debug_audio_start_http_server(), TAG, "start debug http server failed");

    s_debug_audio.initialized = true;
    debug_audio_log_urls();
    return ESP_OK;
}

#elif CONFIG_LK_EXAMPLE_DEBUG_UPLINK_WS || CONFIG_LK_EXAMPLE_DEBUG_DOWNLINK_WS

#define DEBUG_AUDIO_DOWNLINK_RINGBUF_SIZE ((size_t)CONFIG_LK_EXAMPLE_DEBUG_DOWNLINK_RINGBUF_KB * 1024U)
#define DEBUG_AUDIO_WRITER_TASK_STACK_SIZE (4 * 1024)
#define DEBUG_AUDIO_WS_CLIENT_TASK_STACK_SIZE (3 * 1024)

typedef struct {
    const char *name;
    bool ws_connected;
    uint32_t frames_sent;
    uint32_t send_failures;
    uint32_t dropped_frames;
    uint64_t bytes_sent;
    esp_websocket_client_handle_t ws_client;
    TaskHandle_t writer_task;
    RingbufHandle_t ringbuf;
} debug_audio_ws_stream_t;

typedef struct {
    bool initialized;
#if CONFIG_LK_EXAMPLE_DEBUG_UPLINK_WS
    esp_capture_sink_handle_t sink;
#endif
    SemaphoreHandle_t lock;
#if CONFIG_LK_EXAMPLE_DEBUG_UPLINK_WS
    debug_audio_ws_stream_t uplink;
#endif
#if CONFIG_LK_EXAMPLE_DEBUG_DOWNLINK_WS
    debug_audio_ws_stream_t downlink;
#endif
} debug_audio_state_t;

static debug_audio_state_t s_debug_audio;

static BaseType_t debug_audio_create_task(
    TaskFunction_t task_fn,
    const char *task_name,
    uint32_t stack_size,
    void *task_arg,
    UBaseType_t priority,
    TaskHandle_t *task_handle)
{
    BaseType_t ret = xTaskCreateWithCaps(
        task_fn,
        task_name,
        stack_size,
        task_arg,
        priority,
        task_handle,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret == pdPASS) {
        return ret;
    }

    ESP_LOGW(TAG, "Create task %s with PSRAM stack failed, retrying in internal RAM", task_name);
    return xTaskCreate(task_fn, task_name, stack_size, task_arg, priority, task_handle);
}

#if CONFIG_LK_EXAMPLE_DEBUG_UPLINK_WS
static esp_err_t debug_audio_setup_sink(void)
{
    esp_capture_handle_t capturer = livekit_media_get_capturer();
    ESP_RETURN_ON_FALSE(capturer != NULL, ESP_ERR_INVALID_STATE, TAG, "capturer not ready");

    esp_capture_sink_cfg_t sink_cfg = {
        .audio_info = {
            .format_id = ESP_CAPTURE_FMT_ID_PCM,
            .sample_rate = DEBUG_AUDIO_SAMPLE_RATE,
            .channel = DEBUG_AUDIO_CHANNELS,
            .bits_per_sample = DEBUG_AUDIO_BITS_PER_SAMPLE,
        },
    };
    RETURN_ON_CAPTURE_ERROR(
        esp_capture_sink_setup(capturer, 1, &sink_cfg, &s_debug_audio.sink),
        "setup debug sink failed");
    RETURN_ON_CAPTURE_ERROR(
        esp_capture_sink_enable(s_debug_audio.sink, ESP_CAPTURE_RUN_MODE_DISABLE),
        "disable debug sink by default failed");

    ESP_LOGI(TAG, "Processed uplink debug sink configured: PCM 16k mono -> websocket");
    return ESP_OK;
}
#endif

static const char *debug_audio_task_name(const debug_audio_ws_stream_t *stream)
{
    if (stream != NULL && stream->name != NULL && strcmp(stream->name, "downlink") == 0) {
        return "dbg_downlink_ws";
    }
    return "dbg_uplink_ws";
}

static void debug_audio_send_ws_hello(debug_audio_ws_stream_t *stream, esp_websocket_client_handle_t client)
{
    char payload[224];
    int len = snprintf(
        payload,
        sizeof(payload),
        "{\"type\":\"hello\",\"stream\":\"%s\",\"format\":\"pcm_s16le\",\"sample_rate\":%d,"
        "\"channels\":%d,\"bits_per_sample\":%d}",
        (stream != NULL && stream->name != NULL) ? stream->name : "audio",
        DEBUG_AUDIO_SAMPLE_RATE,
        DEBUG_AUDIO_CHANNELS,
        DEBUG_AUDIO_BITS_PER_SAMPLE);
    if (len <= 0 || len >= (int)sizeof(payload)) {
        return;
    }
    int sent = esp_websocket_client_send_text(client, payload, len, pdMS_TO_TICKS(100));
    if (sent < 0) {
        ESP_LOGW(TAG, "Failed to send websocket hello for %s",
            (stream != NULL && stream->name != NULL) ? stream->name : "audio");
    }
}

static void debug_audio_ws_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)base;

    debug_audio_ws_stream_t *stream = (debug_audio_ws_stream_t *)handler_args;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    if (stream == NULL) {
        return;
    }
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            if (xSemaphoreTake(s_debug_audio.lock, portMAX_DELAY) == pdTRUE) {
                stream->ws_connected = true;
                xSemaphoreGive(s_debug_audio.lock);
            }
            ESP_LOGI(TAG, "Debug %s websocket connected", stream->name);
            debug_audio_send_ws_hello(stream, data->client);
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            if (xSemaphoreTake(s_debug_audio.lock, portMAX_DELAY) == pdTRUE) {
                stream->ws_connected = false;
                xSemaphoreGive(s_debug_audio.lock);
            }
            ESP_LOGW(TAG, "Debug %s websocket disconnected", stream->name);
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGW(TAG,
                "Debug %s websocket error: type=%d tls=%s sock_errno=%d",
                stream->name,
                data->error_handle.error_type,
                esp_err_to_name(data->error_handle.esp_tls_last_esp_err),
                data->error_handle.esp_transport_sock_errno);
            break;
        default:
            break;
    }
}

static void debug_audio_note_send(debug_audio_ws_stream_t *stream, int sent)
{
    if (stream == NULL) {
        return;
    }
    if (xSemaphoreTake(s_debug_audio.lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (sent > 0) {
        stream->frames_sent += 1;
        stream->bytes_sent += (uint64_t)sent;
    } else {
        stream->send_failures += 1;
    }
    uint32_t frames = stream->frames_sent;
    uint32_t failures = stream->send_failures;
    uint64_t bytes = stream->bytes_sent;
    xSemaphoreGive(s_debug_audio.lock);

    if (sent > 0) {
        if (frames <= 3 || (frames % 200U) == 0U) {
            ESP_LOGI(TAG, "Debug %s ws frames=%u bytes=%" PRIu64, stream->name, frames, bytes);
        }
    } else if ((failures % 100U) == 1U) {
        ESP_LOGW(TAG, "Debug %s websocket send failed, failures=%u", stream->name, failures);
    }
}

static void debug_audio_note_drop(debug_audio_ws_stream_t *stream)
{
    if (stream == NULL) {
        return;
    }
    if (xSemaphoreTake(s_debug_audio.lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    stream->dropped_frames += 1;
    uint32_t dropped = stream->dropped_frames;
    xSemaphoreGive(s_debug_audio.lock);

    if ((dropped % 100U) == 1U) {
        ESP_LOGW(TAG, "Debug %s dropped frames=%u", stream->name, dropped);
    }
}

static void debug_audio_send_bin(debug_audio_ws_stream_t *stream, const uint8_t *data, size_t size)
{
    if (stream == NULL || data == NULL || size == 0) {
        return;
    }

    bool ws_connected = false;
    esp_websocket_client_handle_t client = NULL;
    if (xSemaphoreTake(s_debug_audio.lock, portMAX_DELAY) == pdTRUE) {
        ws_connected = stream->ws_connected;
        client = stream->ws_client;
        xSemaphoreGive(s_debug_audio.lock);
    }
    if (!ws_connected || client == NULL) {
        return;
    }

    int sent = esp_websocket_client_send_bin(client, (const char *)data, size, 0);
    debug_audio_note_send(stream, sent);
}

#if CONFIG_LK_EXAMPLE_DEBUG_UPLINK_WS
static void debug_audio_uplink_writer_task(void *arg)
{
    debug_audio_ws_stream_t *stream = (debug_audio_ws_stream_t *)arg;

    esp_capture_stream_frame_t frame = {
        .stream_type = ESP_CAPTURE_STREAM_TYPE_AUDIO,
    };

    while (true) {
        esp_capture_err_t ret = esp_capture_sink_acquire_frame(s_debug_audio.sink, &frame, false);

        if (ret == ESP_CAPTURE_ERR_OK) {
            if (frame.data != NULL && frame.size > 0) {
                debug_audio_send_bin(stream, (const uint8_t *)frame.data, (size_t)frame.size);
            }
            esp_capture_sink_release_frame(s_debug_audio.sink, &frame);
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static esp_err_t debug_audio_start_uplink_writer_task_locked(void)
{
    if (s_debug_audio.uplink.writer_task != NULL) {
        return ESP_OK;
    }
    if (debug_audio_create_task(debug_audio_uplink_writer_task,
            debug_audio_task_name(&s_debug_audio.uplink),
            DEBUG_AUDIO_WRITER_TASK_STACK_SIZE,
            &s_debug_audio.uplink,
            4,
            &s_debug_audio.uplink.writer_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
#endif

static esp_err_t debug_audio_start_ws_client(debug_audio_ws_stream_t *stream, const char *url)
{
    ESP_RETURN_ON_FALSE(stream != NULL, ESP_ERR_INVALID_ARG, TAG, "debug stream missing");
    if (url == NULL || url[0] == '\0') {
        ESP_LOGW(TAG, "Debug %s websocket URL is empty", stream->name);
        return ESP_ERR_INVALID_ARG;
    }

    esp_websocket_client_config_t ws_cfg = {
        .uri = url,
        .disable_auto_reconnect = false,
        .reconnect_timeout_ms = CONFIG_LK_EXAMPLE_DEBUG_UPLINK_WS_RECONNECT_MS,
        .network_timeout_ms = 2000,
        .buffer_size = 1024,
        .task_stack = DEBUG_AUDIO_WS_CLIENT_TASK_STACK_SIZE,
        .task_name = debug_audio_task_name(stream),
    };

    stream->ws_client = esp_websocket_client_init(&ws_cfg);
    ESP_RETURN_ON_FALSE(stream->ws_client != NULL, ESP_ERR_NO_MEM, TAG, "init websocket client failed");
    ESP_RETURN_ON_ERROR(
        esp_websocket_register_events(stream->ws_client, WEBSOCKET_EVENT_ANY, debug_audio_ws_event_handler, stream),
        TAG,
        "register websocket events failed");
    ESP_RETURN_ON_ERROR(esp_websocket_client_start(stream->ws_client), TAG, "start websocket client failed");
    ESP_LOGI(TAG, "Streaming %s PCM to %s", stream->name, url);
    return ESP_OK;
}

#if CONFIG_LK_EXAMPLE_DEBUG_UPLINK_WS
static esp_err_t debug_audio_start_uplink_stream(void)
{
    ESP_RETURN_ON_ERROR(debug_audio_start_ws_client(&s_debug_audio.uplink, CONFIG_LK_EXAMPLE_DEBUG_UPLINK_WS_URL),
        TAG,
        "start uplink websocket client failed");
    if (xSemaphoreTake(s_debug_audio.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t task_ret = debug_audio_start_uplink_writer_task_locked();
    xSemaphoreGive(s_debug_audio.lock);
    ESP_RETURN_ON_ERROR(task_ret, TAG, "create websocket writer task failed");

    RETURN_ON_CAPTURE_ERROR(
        esp_capture_sink_enable(s_debug_audio.sink, ESP_CAPTURE_RUN_MODE_ALWAYS),
        "enable debug sink for websocket failed");

    return ESP_OK;
}
#endif

#if CONFIG_LK_EXAMPLE_DEBUG_DOWNLINK_WS
static void debug_audio_downlink_writer_task(void *arg)
{
    debug_audio_ws_stream_t *stream = (debug_audio_ws_stream_t *)arg;

    while (true) {
        size_t item_size = 0;
        uint8_t *item = (uint8_t *)xRingbufferReceive(stream->ringbuf, &item_size, pdMS_TO_TICKS(50));
        if (item == NULL) {
            continue;
        }
        if (item_size > 0) {
            debug_audio_send_bin(stream, item, item_size);
        }
        vRingbufferReturnItem(stream->ringbuf, item);
    }
}

static esp_err_t debug_audio_start_downlink_writer_task_locked(void)
{
    if (s_debug_audio.downlink.writer_task != NULL) {
        return ESP_OK;
    }
    if (debug_audio_create_task(debug_audio_downlink_writer_task,
            debug_audio_task_name(&s_debug_audio.downlink),
            DEBUG_AUDIO_WRITER_TASK_STACK_SIZE,
            &s_debug_audio.downlink,
            4,
            &s_debug_audio.downlink.writer_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t debug_audio_start_downlink_stream(void)
{
    s_debug_audio.downlink.ringbuf = xRingbufferCreateWithCaps(
        DEBUG_AUDIO_DOWNLINK_RINGBUF_SIZE,
        RINGBUF_TYPE_BYTEBUF,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_debug_audio.downlink.ringbuf != NULL, ESP_ERR_NO_MEM, TAG, "create downlink ringbuf failed");
    ESP_LOGI(TAG,
        "Debug downlink ring buffer=%uKB",
        (unsigned)CONFIG_LK_EXAMPLE_DEBUG_DOWNLINK_RINGBUF_KB);
    ESP_RETURN_ON_ERROR(debug_audio_start_ws_client(&s_debug_audio.downlink, CONFIG_LK_EXAMPLE_DEBUG_DOWNLINK_WS_URL),
        TAG,
        "start downlink websocket client failed");
    if (xSemaphoreTake(s_debug_audio.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t task_ret = debug_audio_start_downlink_writer_task_locked();
    xSemaphoreGive(s_debug_audio.lock);
    ESP_RETURN_ON_ERROR(task_ret, TAG, "create downlink websocket writer task failed");
    return ESP_OK;
}
#endif

esp_err_t livekit_debug_audio_init(void)
{
    if (s_debug_audio.initialized) {
        return ESP_OK;
    }

    memset(&s_debug_audio, 0, sizeof(s_debug_audio));
    s_debug_audio.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_debug_audio.lock != NULL, ESP_ERR_NO_MEM, TAG, "create debug mutex failed");

#if CONFIG_LK_EXAMPLE_DEBUG_UPLINK_WS
    s_debug_audio.uplink.name = "uplink";
    ESP_RETURN_ON_ERROR(debug_audio_setup_sink(), TAG, "setup debug sink failed");
    ESP_RETURN_ON_ERROR(debug_audio_start_uplink_stream(), TAG, "start websocket uplink stream failed");
#endif
#if CONFIG_LK_EXAMPLE_DEBUG_DOWNLINK_WS
    s_debug_audio.downlink.name = "downlink";
    ESP_RETURN_ON_ERROR(debug_audio_start_downlink_stream(), TAG, "start websocket downlink stream failed");
#endif

    s_debug_audio.initialized = true;
    return ESP_OK;
}

esp_err_t livekit_debug_audio_start_recording(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

int livekit_debug_audio_on_downlink_pcm(uint8_t *data, int size, void *ctx)
{
    (void)ctx;
#if CONFIG_LK_EXAMPLE_DEBUG_DOWNLINK_WS
    if (!s_debug_audio.initialized || s_debug_audio.downlink.ringbuf == NULL || data == NULL || size <= 0) {
        return 0;
    }
    if (xRingbufferSend(s_debug_audio.downlink.ringbuf, data, (size_t)size, 0) != pdTRUE) {
        debug_audio_note_drop(&s_debug_audio.downlink);
    }
#else
    (void)data;
    (void)size;
#endif
    return 0;
}

#else

esp_err_t livekit_debug_audio_init(void)
{
    return ESP_OK;
}

esp_err_t livekit_debug_audio_start_recording(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

int livekit_debug_audio_on_downlink_pcm(uint8_t *data, int size, void *ctx)
{
    (void)data;
    (void)size;
    (void)ctx;
    return 0;
}

#endif
