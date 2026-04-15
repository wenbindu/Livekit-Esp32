#include "livekit_app.h"

#include <cJSON.h>
#include <stdbool.h>
#include <time.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <string.h>

#include "app_diagnostics.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_mac.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_timer.h"
#include "livekit.h"
#include "livekit_sandbox.h"
#include "lichuang_ui.h"
#include "livekit_media.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"

#ifdef CONFIG_LK_EXAMPLE_LIVEKIT_PUBLISHER_DATA_CHANNEL
#define LK_PUBLISHER_DATA_CHANNEL_ENABLED 1
#else
#define LK_PUBLISHER_DATA_CHANNEL_ENABLED 0
#endif

static const char *TAG = "livekit_room";
static livekit_room_handle_t s_room_handle;
static esp_err_t http_response_buffer_event_handler(esp_http_client_event_t *event);
#if CONFIG_LK_EXAMPLE_USE_DEVICE_JWT
static void request_agent_dispatch_if_needed(void);
#endif
static void destroy_room(void);
static void on_state_changed(livekit_connection_state_t state, void *ctx);
static void on_room_info(const livekit_room_info_t *info, void *ctx);
static void on_participant_info(const livekit_participant_info_t *info, void *ctx);
static void on_data_received(const livekit_data_received_t *data, void *ctx);
#if CONFIG_LK_EXAMPLE_USE_TOKEN_SERVER
static bool schedule_token_server_connect_task(uint32_t delay_ms);
#endif

#define TOKEN_SERVER_RESPONSE_MAX 8192
#define TOKEN_SERVER_URL_MAX 256
#define TOKEN_SERVER_TOKEN_MAX 4096
#define LIVEKIT_JWT_HMAC_SIZE 32
#define AGENT_DISPATCH_RESPONSE_MAX 1024
#define AGENT_DISPATCH_DELAY_MS 3000
#define AGENT_DISPATCH_DELAY_TASK_STACK_SIZE 4096
#define AGENT_DISPATCH_DELAY_TASK_PRIORITY 3
#define AGENT_DISPATCH_TASK_STACK_SIZE 16384
#define AGENT_DISPATCH_TASK_PRIORITY 4
#define TOKEN_SERVER_CONNECT_TASK_STACK_SIZE 16384
#define TOKEN_SERVER_CONNECT_TASK_PRIORITY 4

typedef enum {
    TOKEN_SERVER_FETCH_RESULT_OK = 0,
    TOKEN_SERVER_FETCH_RESULT_TRANSPORT,
    TOKEN_SERVER_FETCH_RESULT_HTTP_UNAUTHORIZED,
    TOKEN_SERVER_FETCH_RESULT_HTTP_CLIENT_ERROR,
    TOKEN_SERVER_FETCH_RESULT_HTTP_SERVER_ERROR,
    TOKEN_SERVER_FETCH_RESULT_INVALID_RESPONSE,
} token_server_fetch_result_t;

typedef struct {
    char url[TOKEN_SERVER_URL_MAX];
    char room_name[64];
    char room_sid[32];
    char agent_name[64];
} agent_dispatch_task_args_t;

typedef struct {
    bool agent_active;
    bool agent_dispatch_scheduled;
    bool agent_dispatch_requested;
    bool agent_dispatch_inflight;
    bool stop_requested;
    bool token_connect_task_running;
    bool token_refresh_pending;
    char room_sid[32];
    char room_name[48];
    char agent_identity[48];
    char agent_name[48];
    char last_user_text[192];
    char last_assistant_text[192];
    char current_emoji[16];
    int64_t last_text_us;
    uint8_t auth_failure_count;
    uint32_t connect_generation;
} livekit_runtime_state_t;

typedef struct {
    char body[TOKEN_SERVER_RESPONSE_MAX];
    size_t len;
} http_response_buffer_t;

typedef struct {
    char server_url[TOKEN_SERVER_URL_MAX];
    char token[TOKEN_SERVER_TOKEN_MAX];
} fetched_room_credentials_t;

typedef struct {
    token_server_fetch_result_t result;
    int http_status;
    char error_text[96];
} token_server_fetch_diag_t;

typedef struct {
    uint32_t generation;
    uint32_t delay_ms;
} token_server_connect_task_args_t;

static livekit_runtime_state_t s_runtime;
static portMUX_TYPE s_runtime_mux = portMUX_INITIALIZER_UNLOCKED;

static void *alloc_internal_zeroed(size_t size)
{
    return heap_caps_calloc(1, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void *alloc_preferred_zeroed(size_t size)
{
    return heap_caps_calloc_prefer(
        1,
        size,
        2,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void log_heap_state(const char *stage)
{
    const uint32_t internal_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t internal_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t internal_min = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t spiram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    ESP_LOGI(TAG,
        "Heap[%s]: internal_free=%" PRIu32 " internal_largest=%" PRIu32 " internal_min=%" PRIu32 " spiram_free=%" PRIu32,
        stage != NULL ? stage : "?",
        internal_free,
        internal_largest,
        internal_min,
        spiram_free);
}

static esp_err_t http_response_buffer_event_handler(esp_http_client_event_t *event)
{
    http_response_buffer_t *buffer = (http_response_buffer_t *)event->user_data;
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

static const char *active_participant_identity(void)
{
    return strlen(CONFIG_LK_EXAMPLE_PARTICIPANT_IDENTITY) > 0
        ? CONFIG_LK_EXAMPLE_PARTICIPANT_IDENTITY
        : CONFIG_LK_EXAMPLE_PARTICIPANT_NAME;
}

static const char *participant_kind_str(livekit_participant_kind_t kind)
{
    switch (kind) {
    case LIVEKIT_PARTICIPANT_KIND_STANDARD:
        return "standard";
    case LIVEKIT_PARTICIPANT_KIND_INGRESS:
        return "ingress";
    case LIVEKIT_PARTICIPANT_KIND_EGRESS:
        return "egress";
    case LIVEKIT_PARTICIPANT_KIND_SIP:
        return "sip";
    case LIVEKIT_PARTICIPANT_KIND_AGENT:
        return "agent";
    default:
        return "unknown";
    }
}

static const char *participant_state_str(livekit_participant_state_t state)
{
    switch (state) {
    case LIVEKIT_PARTICIPANT_STATE_JOINING:
        return "joining";
    case LIVEKIT_PARTICIPANT_STATE_JOINED:
        return "joined";
    case LIVEKIT_PARTICIPANT_STATE_ACTIVE:
        return "active";
    case LIVEKIT_PARTICIPANT_STATE_DISCONNECTED:
        return "disconnected";
    default:
        return "unknown";
    }
}

static const char *active_room_name(void)
{
    return s_runtime.room_name[0] != '\0' ? s_runtime.room_name : CONFIG_LK_EXAMPLE_ROOM_NAME;
}

#if CONFIG_LK_EXAMPLE_USE_DEVICE_JWT
static bool active_room_sid_matches(const char *room_sid)
{
    return room_sid != NULL &&
        room_sid[0] != '\0' &&
        s_runtime.room_sid[0] != '\0' &&
        strcmp(s_runtime.room_sid, room_sid) == 0;
}
#endif

static void ensure_runtime_room_name(void)
{
    if (s_runtime.room_name[0] != '\0') {
        return;
    }

#if CONFIG_LK_EXAMPLE_USE_SANDBOX
    uint8_t mac[6] = {0};
    time_t now = time(NULL);
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK && now > 0) {
        int written = snprintf(
            s_runtime.room_name,
            sizeof(s_runtime.room_name),
            "%s-%02x%02x%02x-%lu",
            CONFIG_LK_EXAMPLE_ROOM_NAME,
            mac[3],
            mac[4],
            mac[5],
            (unsigned long)(now % 1000000UL));
        if (written > 0 && written < (int)sizeof(s_runtime.room_name)) {
            ESP_LOGI(TAG, "Using sandbox runtime room name: %s", s_runtime.room_name);
            return;
        }
    }
#endif

    strlcpy(s_runtime.room_name, CONFIG_LK_EXAMPLE_ROOM_NAME, sizeof(s_runtime.room_name));
    ESP_LOGI(TAG, "Using configured room name: %s", s_runtime.room_name);
}

static void initialize_runtime_state(void)
{
    memset(&s_runtime, 0, sizeof(s_runtime));
    strlcpy(s_runtime.current_emoji, ":)", sizeof(s_runtime.current_emoji));
    ensure_runtime_room_name();
}

static void reset_connection_runtime_state(bool clear_chat_history)
{
    s_runtime.agent_active = false;
    s_runtime.agent_dispatch_scheduled = false;
    s_runtime.agent_dispatch_requested = false;
    s_runtime.agent_dispatch_inflight = false;
    s_runtime.room_sid[0] = '\0';
    s_runtime.agent_identity[0] = '\0';
    s_runtime.agent_name[0] = '\0';

    if (clear_chat_history) {
        s_runtime.last_user_text[0] = '\0';
        s_runtime.last_assistant_text[0] = '\0';
        s_runtime.last_text_us = 0;
    }
    if (clear_chat_history || s_runtime.current_emoji[0] == '\0') {
        strlcpy(s_runtime.current_emoji, ":)", sizeof(s_runtime.current_emoji));
    }
}

static bool runtime_is_stop_requested(void)
{
    bool stop_requested = false;

    taskENTER_CRITICAL(&s_runtime_mux);
    stop_requested = s_runtime.stop_requested;
    taskEXIT_CRITICAL(&s_runtime_mux);
    return stop_requested;
}

static void runtime_set_stop_requested(bool stop_requested)
{
    taskENTER_CRITICAL(&s_runtime_mux);
    s_runtime.stop_requested = stop_requested;
    if (stop_requested) {
        s_runtime.token_refresh_pending = false;
    }
    taskEXIT_CRITICAL(&s_runtime_mux);
}

static bool runtime_begin_token_connect_task(uint32_t *generation_out)
{
    bool ok = false;

    taskENTER_CRITICAL(&s_runtime_mux);
    if (!s_runtime.stop_requested && !s_runtime.token_connect_task_running) {
        s_runtime.token_connect_task_running = true;
        s_runtime.token_refresh_pending = true;
        s_runtime.connect_generation++;
        if (generation_out != NULL) {
            *generation_out = s_runtime.connect_generation;
        }
        ok = true;
    }
    taskEXIT_CRITICAL(&s_runtime_mux);
    return ok;
}

static void runtime_finish_token_connect_task(void)
{
    taskENTER_CRITICAL(&s_runtime_mux);
    s_runtime.token_connect_task_running = false;
    taskEXIT_CRITICAL(&s_runtime_mux);
}

static bool runtime_is_current_generation(uint32_t generation)
{
    bool current = false;

    taskENTER_CRITICAL(&s_runtime_mux);
    current = generation == s_runtime.connect_generation;
    taskEXIT_CRITICAL(&s_runtime_mux);
    return current;
}

static void runtime_clear_auth_failure_state(void)
{
    taskENTER_CRITICAL(&s_runtime_mux);
    s_runtime.auth_failure_count = 0;
    s_runtime.token_refresh_pending = false;
    taskEXIT_CRITICAL(&s_runtime_mux);
}

static uint8_t runtime_record_auth_failure(void)
{
    uint8_t failure_count = 0;

    taskENTER_CRITICAL(&s_runtime_mux);
    if (s_runtime.auth_failure_count < UINT8_MAX) {
        s_runtime.auth_failure_count++;
    }
    failure_count = s_runtime.auth_failure_count;
    s_runtime.token_refresh_pending = true;
    taskEXIT_CRITICAL(&s_runtime_mux);
    return failure_count;
}

static void runtime_mark_token_refresh_pending(void)
{
    taskENTER_CRITICAL(&s_runtime_mux);
    s_runtime.token_refresh_pending = true;
    taskEXIT_CRITICAL(&s_runtime_mux);
}

static void runtime_clear_token_refresh_pending(void)
{
    taskENTER_CRITICAL(&s_runtime_mux);
    s_runtime.token_refresh_pending = false;
    taskEXIT_CRITICAL(&s_runtime_mux);
}

static bool runtime_token_refresh_pending(void)
{
    bool pending = false;

    taskENTER_CRITICAL(&s_runtime_mux);
    pending = s_runtime.token_refresh_pending;
    taskEXIT_CRITICAL(&s_runtime_mux);
    return pending;
}

static void format_agent_label(char *out, size_t out_size)
{
    strlcpy(out, "AI", out_size);
}

static bool payload_is_text(const uint8_t *bytes, size_t size)
{
    if (bytes == NULL || size == 0) {
        return false;
    }

    size_t text_like = 0;
    for (size_t i = 0; i < size; ++i) {
        uint8_t ch = bytes[i];
        if ((ch >= 32 && ch <= 126) || ch >= 0x80 || ch == '\n' || ch == '\r' || ch == '\t') {
            text_like++;
        }
    }
    return text_like * 10 >= size * 8;
}

static void normalize_payload_text(char *out, size_t out_size, const uint8_t *bytes, size_t size)
{
    if (out == NULL || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (bytes == NULL || size == 0) {
        return;
    }

    size_t written = 0;
    bool previous_space = true;
    for (size_t i = 0; i < size && written + 1 < out_size; ++i) {
        uint8_t ch = bytes[i];
        if (ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ') {
            if (!previous_space && written + 1 < out_size) {
                out[written++] = ' ';
                previous_space = true;
            }
            continue;
        }
        if (ch < 0x20) {
            continue;
        }
        out[written++] = (char)ch;
        previous_space = false;
    }

    while (written > 0 && out[written - 1] == ' ') {
        written--;
    }
    out[written] = '\0';
}

static const char *emoji_for_emotion(const char *emotion)
{
    if (emotion == NULL || emotion[0] == '\0') {
        return ":)";
    }
    if (strcmp(emotion, "neutral") == 0 || strcmp(emotion, "calm") == 0) {
        return ":|";
    }
    if (strcmp(emotion, "happy") == 0 || strcmp(emotion, "smile") == 0) {
        return ":)";
    }
    if (strcmp(emotion, "laughing") == 0 || strcmp(emotion, "excited") == 0) {
        return ":D";
    }
    if (strcmp(emotion, "sad") == 0 || strcmp(emotion, "crying") == 0) {
        return ":(";
    }
    if (strcmp(emotion, "wink") == 0) {
        return ";)";
    }
    if (strcmp(emotion, "cool") == 0 || strcmp(emotion, "confident") == 0) {
        return "B)";
    }
    if (strcmp(emotion, "love") == 0 || strcmp(emotion, "loving") == 0) {
        return "<3";
    }
    if (strcmp(emotion, "playful") == 0 || strcmp(emotion, "silly") == 0) {
        return ":P";
    }
    return ":)";
}

static bool sender_is_agent(const char *sender)
{
    if (sender == NULL || sender[0] == '\0') {
        return false;
    }
    if (s_runtime.agent_identity[0] != '\0' && strcmp(sender, s_runtime.agent_identity) == 0) {
        return true;
    }
    if (s_runtime.agent_name[0] != '\0' && strcmp(sender, s_runtime.agent_name) == 0) {
        return true;
    }
    return strncmp(sender, "agent-", 6) == 0;
}

static bool is_internal_topic(const char *topic)
{
    return topic != NULL && strncmp(topic, "lk.", 3) == 0;
}

static bool have_chat_history(void)
{
    return s_runtime.last_user_text[0] != '\0' || s_runtime.last_assistant_text[0] != '\0';
}

static bool is_utf8_continuation_byte(unsigned char ch)
{
    return (ch & 0xC0U) == 0x80U;
}

static size_t utf8_sequence_length(unsigned char lead)
{
    if ((lead & 0x80U) == 0) {
        return 1;
    }
    if ((lead & 0xE0U) == 0xC0U) {
        return 2;
    }
    if ((lead & 0xF0U) == 0xE0U) {
        return 3;
    }
    if ((lead & 0xF8U) == 0xF0U) {
        return 4;
    }
    return 1;
}

static void sanitize_display_text(char *out, size_t out_size, const char *text)
{
    if (out == NULL || out_size == 0) {
        return;
    }

    out[0] = '\0';
    if (text == NULL || text[0] == '\0') {
        return;
    }

    size_t written = 0;
    bool previous_space = true;
    const unsigned char *src = (const unsigned char *)text;
    while (*src != '\0' && written + 1 < out_size) {
        if (src[0] == '\r' || src[0] == '\n' || src[0] == '\t') {
            if (!previous_space) {
                out[written++] = ' ';
                previous_space = true;
            }
            src++;
            continue;
        }

        if (src[0] == 0xEF && src[1] != '\0' && src[2] != '\0' &&
            src[1] == 0xBB && src[2] == 0xBF) {
            src += 3;
            continue;
        }

        if (src[0] == 0xE2 && src[1] != '\0' && src[2] != '\0' &&
            src[1] == 0x80 &&
            (src[2] == 0x8B || src[2] == 0x8C || src[2] == 0x8D || src[2] == 0x8E ||
             src[2] == 0x8F || src[2] == 0xAA)) {
            src += 3;
            continue;
        }

        if (src[0] == 0xEF && src[1] != '\0' && src[2] != '\0' &&
            src[1] == 0xB8 && (src[2] == 0x8E || src[2] == 0x8F)) {
            src += 3;
            continue;
        }

        if (src[0] == 0xE2 && src[1] != '\0' && src[2] != '\0' && src[1] == 0x80 &&
            (src[2] == 0x98 || src[2] == 0x99 || src[2] == 0x9A || src[2] == 0x9B ||
             src[2] == 0x9C || src[2] == 0x9D || src[2] == 0x9E || src[2] == 0x9F ||
             src[2] == 0xB2 || src[2] == 0xB3)) {
            out[written++] = (src[2] == 0x9C || src[2] == 0x9D || src[2] == 0x9E ||
                              src[2] == 0x9F || src[2] == 0xB3) ? '"' : '\'';
            previous_space = false;
            src += 3;
            continue;
        }

        if (src[0] == 0xE2 && src[1] != '\0' && src[2] != '\0' &&
            src[1] == 0x80 && (src[2] == 0x93 || src[2] == 0x94)) {
            out[written++] = '-';
            previous_space = false;
            src += 3;
            continue;
        }

        if (src[0] == 0xE2 && src[1] != '\0' && src[2] != '\0' &&
            src[1] == 0x80 && (src[2] == 0xA2 || src[2] == 0xA3 || src[2] == 0xA4 || src[2] == 0xA7)) {
            out[written++] = '-';
            previous_space = false;
            src += 3;
            continue;
        }

        if (src[0] == 0xC2 && src[1] != '\0' && (src[1] == 0xAB || src[1] == 0xBB)) {
            out[written++] = '"';
            previous_space = false;
            src += 2;
            continue;
        }

        if (src[0] == 0xE2 && src[1] != '\0' && src[2] != '\0' &&
            src[1] == 0x80 && src[2] == 0xA6) {
            if (written + 3 < out_size) {
                out[written++] = '.';
                out[written++] = '.';
                out[written++] = '.';
            }
            previous_space = false;
            src += 3;
            continue;
        }

        if (src[0] == 0xC2 && src[1] != '\0' && src[1] == 0xA0) {
            if (!previous_space) {
                out[written++] = ' ';
                previous_space = true;
            }
            src += 2;
            continue;
        }

        if (src[0] == 0xE3 && src[1] != '\0' && src[2] != '\0' &&
            src[1] == 0x80 &&
            (src[2] == 0x8C || src[2] == 0x8D || src[2] == 0x8E || src[2] == 0x8F ||
             src[2] == 0x9D || src[2] == 0x9E || src[2] == 0x9F)) {
            out[written++] = '"';
            previous_space = false;
            src += 3;
            continue;
        }

        if (src[0] == 0xEF && src[1] != '\0' && src[2] != '\0' &&
            src[1] == 0xBC && (src[2] == 0x82 || src[2] == 0x87)) {
            out[written++] = (src[2] == 0x82) ? '"' : '\'';
            previous_space = false;
            src += 3;
            continue;
        }

        if (src[0] == 0xEF && src[1] != '\0' && src[2] != '\0' && src[1] == 0xBF && src[2] == 0xBD) {
            src += 3;
            continue;
        }

        if (src[0] < 0x20) {
            src++;
            continue;
        }

        if (src[0] >= 0x80) {
            src += utf8_sequence_length(src[0]);
            continue;
        }

        out[written++] = (char)src[0];
        previous_space = (src[0] == ' ');
        src++;
    }

    while (written > 0 && out[written - 1] == ' ') {
        written--;
    }
    out[written] = '\0';
}

static void copy_recent_text(char *out, size_t out_size, const char *text, size_t max_chars)
{
    if (out == NULL || out_size == 0) {
        return;
    }

    out[0] = '\0';
    if (text == NULL || text[0] == '\0') {
        return;
    }

    char sanitized[192];
    sanitize_display_text(sanitized, sizeof(sanitized), text);

    size_t len = strlen(sanitized);
    if (len <= max_chars || max_chars < 8) {
        strlcpy(out, sanitized, out_size);
        return;
    }

    const char *start = sanitized + (len - max_chars);
    while (is_utf8_continuation_byte((unsigned char)*start)) {
        start++;
    }
    while (*start != '\0' && *start != ' ' && (size_t)(sanitized + len - start) > (max_chars * 3 / 4)) {
        start++;
    }
    while (is_utf8_continuation_byte((unsigned char)*start)) {
        start++;
    }
    while (*start == ' ') {
        start++;
    }

    strlcpy(out, "...", out_size);
    strlcat(out, start, out_size);
}

static void format_chat_line(
    char *out,
    size_t out_size,
    const char *speaker,
    const char *text,
    const char *fallback,
    size_t max_chars)
{
    char recent[160];
    copy_recent_text(recent, sizeof(recent), text, max_chars);

    strlcpy(out, speaker, out_size);
    strlcat(out, ": ", out_size);
    if (recent[0] != '\0') {
        strlcat(out, recent, out_size);
    } else if (fallback != NULL && fallback[0] != '\0') {
        strlcat(out, fallback, out_size);
    }
}

static void refresh_chat_ui(void)
{
    char primary[192];
    char secondary[208];
    format_chat_line(
        primary,
        sizeof(primary),
        "User",
        s_runtime.last_user_text,
        s_runtime.agent_active ? "Listening..." : "Waiting...",
        42);
    format_chat_line(
        secondary,
        sizeof(secondary),
        "AI",
        s_runtime.last_assistant_text,
        s_runtime.agent_active ? "Thinking..." : "Ready...",
        58);

    lichuang_ui_show_message(
        "LIVEKIT",
        primary,
        secondary,
        s_runtime.current_emoji[0] != '\0' ? s_runtime.current_emoji : ":)");
}

static void update_user_text(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }
    sanitize_display_text(s_runtime.last_user_text, sizeof(s_runtime.last_user_text), text);
    s_runtime.last_text_us = esp_timer_get_time();
    refresh_chat_ui();
}

static void update_assistant_text(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }
    sanitize_display_text(s_runtime.last_assistant_text, sizeof(s_runtime.last_assistant_text), text);
    if (s_runtime.current_emoji[0] == '\0') {
        strlcpy(s_runtime.current_emoji, ":)", sizeof(s_runtime.current_emoji));
    }
    s_runtime.last_text_us = esp_timer_get_time();
    refresh_chat_ui();
}

static void update_emotion(const char *emotion)
{
    strlcpy(s_runtime.current_emoji, emoji_for_emotion(emotion), sizeof(s_runtime.current_emoji));
    if (have_chat_history()) {
        refresh_chat_ui();
    }
}

static bool handle_json_payload(const char *payload)
{
    if (payload == NULL) {
        return false;
    }

    while (*payload == ' ') {
        payload++;
    }
    if (*payload != '{') {
        return false;
    }

    cJSON *root = cJSON_Parse(payload);
    if (root == NULL) {
        return false;
    }

    bool handled = false;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return false;
    }

    if (strcmp(type->valuestring, "tts") == 0) {
        const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(state) && strcmp(state->valuestring, "sentence_start") == 0 && cJSON_IsString(text)) {
            update_assistant_text(text->valuestring);
        }
        handled = true;
    } else if (strcmp(type->valuestring, "stt") == 0) {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text)) {
            update_user_text(text->valuestring);
        }
        handled = true;
    } else if (strcmp(type->valuestring, "llm") == 0) {
        const cJSON *emotion = cJSON_GetObjectItemCaseSensitive(root, "emotion");
        if (cJSON_IsString(emotion)) {
            update_emotion(emotion->valuestring);
        }
        handled = true;
    } else if (strcmp(type->valuestring, "alert") == 0) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "message");
        const cJSON *emotion = cJSON_GetObjectItemCaseSensitive(root, "emotion");
        if (cJSON_IsString(message)) {
            update_assistant_text(message->valuestring);
        }
        if (cJSON_IsString(emotion)) {
            update_emotion(emotion->valuestring);
        }
        handled = true;
    }

    cJSON_Delete(root);
    return handled;
}

static void format_payload_preview(char *out, size_t out_size, const uint8_t *bytes, size_t size)
{
    if (out_size == 0) {
        return;
    }
    if (bytes == NULL || size == 0) {
        strlcpy(out, "EMPTY PAYLOAD", out_size);
        return;
    }

    size_t written = 0;
    size_t limit = size < (out_size - 1) ? size : (out_size - 1);
    for (size_t i = 0; i < limit; ++i) {
        uint8_t ch = bytes[i];
        if (ch == '\n' || ch == '\r' || ch == '\t') {
            ch = ' ';
        }
        out[written++] = (ch >= 32 && ch <= 126) ? (char)ch : '.';
    }
    out[written] = '\0';
}

static bool text_has_alpha(const char *text)
{
    if (text == NULL) {
        return false;
    }
    for (const char *p = text; *p != '\0'; ++p) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
            return true;
        }
    }
    return false;
}

static bool text_looks_like_session_noise(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return true;
    }

    return strstr(text, "livekit") != NULL ||
        strstr(text, "openai/") != NULL ||
        strstr(text, "cartesia/") != NULL ||
        strstr(text, "gpt-4") != NULL ||
        strstr(text, "sonic-3") != NULL ||
        strstr(text, "item_") != NULL;
}

static bool extract_agent_session_text(char *out, size_t out_size, const uint8_t *bytes, size_t size)
{
    if (out == NULL || out_size == 0) {
        return false;
    }
    out[0] = '\0';

    if (bytes == NULL || size == 0) {
        return false;
    }

    char best[192] = {0};
    int best_score = 0;

    size_t span_start = 0;
    while (span_start < size) {
        while (span_start < size && (bytes[span_start] < 32 || bytes[span_start] > 126)) {
            span_start++;
        }
        if (span_start >= size) {
            break;
        }

        size_t span_end = span_start;
        while (span_end < size && bytes[span_end] >= 32 && bytes[span_end] <= 126) {
            span_end++;
        }

        char candidate[192];
        normalize_payload_text(candidate, sizeof(candidate), bytes + span_start, span_end - span_start);
        size_t len = strlen(candidate);
        if (len >= 12 && text_has_alpha(candidate) && strstr(candidate, " ") != NULL &&
            !text_looks_like_session_noise(candidate)) {
            int score = (int)len;
            if (strchr(candidate, '?') != NULL || strchr(candidate, '!') != NULL || strchr(candidate, '.') != NULL) {
                score += 16;
            }
            if (score > best_score) {
                best_score = score;
                strlcpy(best, candidate, sizeof(best));
            }
        }

        span_start = span_end + 1;
    }

    if (best_score <= 0) {
        return false;
    }

    strlcpy(out, best, out_size);
    return true;
}

static bool is_fake_ipv4_text(const char *ip_text)
{
    return strncmp(ip_text, "198.18.", 7) == 0 || strncmp(ip_text, "198.19.", 7) == 0;
}

static const char *dns_type_str(esp_netif_dns_type_t type)
{
    switch (type) {
    case ESP_NETIF_DNS_MAIN:
        return "main";
    case ESP_NETIF_DNS_BACKUP:
        return "backup";
    case ESP_NETIF_DNS_FALLBACK:
        return "fallback";
    default:
        return "unknown";
    }
}

static void log_dns_entry(esp_netif_t *netif, esp_netif_dns_type_t type)
{
    esp_netif_dns_info_t dns = {};
    esp_err_t err = esp_netif_get_dns_info(netif, type, &dns);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DNS[%s]: unavailable (%s)", dns_type_str(type), esp_err_to_name(err));
        return;
    }

    if (dns.ip.type == ESP_IPADDR_TYPE_V4) {
        ESP_LOGI(TAG, "DNS[%s]: " IPSTR,
            dns_type_str(type),
            IP2STR(&dns.ip.u_addr.ip4));
        return;
    }

    if (dns.ip.type == ESP_IPADDR_TYPE_V6) {
        ESP_LOGI(TAG, "DNS[%s]: " IPV6STR,
            dns_type_str(type),
            IPV62STR(dns.ip.u_addr.ip6));
        return;
    }

    ESP_LOGI(TAG, "DNS[%s]: type=%u", dns_type_str(type), dns.ip.type);
}

static void log_wifi_snapshot(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        ESP_LOGW(TAG, "Wi-Fi netif WIFI_STA_DEF not available");
        return;
    }

    esp_netif_ip_info_t ip_info = {};
    if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi IPv4: ip=" IPSTR " mask=" IPSTR " gw=" IPSTR,
            IP2STR(&ip_info.ip), IP2STR(&ip_info.netmask), IP2STR(&ip_info.gw));
    } else {
        ESP_LOGW(TAG, "Unable to read Wi-Fi IPv4 info");
    }

    log_dns_entry(netif, ESP_NETIF_DNS_MAIN);
    log_dns_entry(netif, ESP_NETIF_DNS_BACKUP);
    log_dns_entry(netif, ESP_NETIF_DNS_FALLBACK);
}

static bool extract_host(const char *source, char *host, size_t host_size)
{
    if (source == NULL || host == NULL || host_size < 2) {
        return false;
    }

    const char *start = strstr(source, "://");
    if (start != NULL) {
        start += 3;
    } else {
        start = strchr(source, ':');
        if (start == NULL) {
            return false;
        }
        start += 1;
    }

    if (*start == '[') {
        const char *end = strchr(start + 1, ']');
        if (end == NULL) {
            return false;
        }
        size_t len = (size_t)(end - (start + 1));
        if (len == 0 || len >= host_size) {
            return false;
        }
        memcpy(host, start + 1, len);
        host[len] = '\0';
        return true;
    }

    const char *end = start;
    while (*end != '\0' && *end != ':' && *end != '/' && *end != '?' && *end != '#') {
        end++;
    }

    size_t len = (size_t)(end - start);
    if (len == 0 || len >= host_size) {
        return false;
    }

    memcpy(host, start, len);
    host[len] = '\0';
    return true;
}

static bool derive_turn_host(const char *server_host, char *turn_host, size_t turn_host_size)
{
    const char *suffix = ".livekit.cloud";
    size_t host_len = strlen(server_host);
    size_t suffix_len = strlen(suffix);

    if (host_len <= suffix_len || strcmp(server_host + host_len - suffix_len, suffix) != 0) {
        return false;
    }

    int prefix_len = (int)(host_len - suffix_len);
    int written = snprintf(turn_host, turn_host_size, "%.*s.turn.livekit.cloud", prefix_len, server_host);
    return written > 0 && (size_t)written < turn_host_size;
}

static void log_host_resolution(const char *label, const char *host)
{
    struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *result = NULL;
    int err = getaddrinfo(host, NULL, &hints, &result);

    if (err != 0) {
        ESP_LOGW(TAG, "Resolve[%s] host=%s failed: %d", label, host, err);
        return;
    }

    bool found = false;
    int index = 0;
    for (const struct addrinfo *it = result; it != NULL; it = it->ai_next) {
        char ip_text[INET6_ADDRSTRLEN] = {0};
        const void *addr = NULL;

        if (it->ai_family == AF_INET) {
            addr = &((const struct sockaddr_in *)it->ai_addr)->sin_addr;
        } else if (it->ai_family == AF_INET6) {
            addr = &((const struct sockaddr_in6 *)it->ai_addr)->sin6_addr;
        } else {
            continue;
        }

        if (inet_ntop(it->ai_family, addr, ip_text, sizeof(ip_text)) == NULL) {
            continue;
        }

        found = true;
        if (is_fake_ipv4_text(ip_text)) {
            ESP_LOGW(TAG, "Resolve[%s] host=%s -> %s [FAKE-IP suspect]", label, host, ip_text);
        } else {
            ESP_LOGI(TAG, "Resolve[%s] host=%s -> %s", label, host, ip_text);
        }
        index++;
    }

    if (!found) {
        ESP_LOGW(TAG, "Resolve[%s] host=%s returned no usable addresses", label, host);
    }
    freeaddrinfo(result);
}

static void log_livekit_network_diagnostics(const char *server_url)
{
    char host[96];
    char turn_host[112];

    log_wifi_snapshot();

#if CONFIG_LK_EXAMPLE_USE_SANDBOX
    log_host_resolution("sandbox_api", "cloud-api.livekit.io");
#endif

    if (server_url == NULL) {
        return;
    }

    if (!extract_host(server_url, host, sizeof(host))) {
        ESP_LOGW(TAG, "Unable to extract host from server_url=%s", server_url);
        return;
    }

    ESP_LOGI(TAG, "LiveKit server_url=%s", server_url);
    log_host_resolution("signal", host);

    if (derive_turn_host(host, turn_host, sizeof(turn_host))) {
        log_host_resolution("project_turn", turn_host);
    }
}

#if CONFIG_LK_EXAMPLE_USE_TOKEN_SERVER
static void log_token_server_diagnostics(const char *token_server_url)
{
    char host[96];

    log_wifi_snapshot();
    if (token_server_url == NULL || !extract_host(token_server_url, host, sizeof(host))) {
        ESP_LOGW(TAG, "Unable to extract host from token_server_url=%s",
            token_server_url != NULL ? token_server_url : "(null)");
        return;
    }

    ESP_LOGI(TAG, "Token server url=%s", token_server_url);
    log_host_resolution("token_server", host);
}

static const char *token_server_fetch_result_str(token_server_fetch_result_t result)
{
    switch (result) {
    case TOKEN_SERVER_FETCH_RESULT_OK:
        return "ok";
    case TOKEN_SERVER_FETCH_RESULT_TRANSPORT:
        return "transport";
    case TOKEN_SERVER_FETCH_RESULT_HTTP_UNAUTHORIZED:
        return "http_unauthorized";
    case TOKEN_SERVER_FETCH_RESULT_HTTP_CLIENT_ERROR:
        return "http_client_error";
    case TOKEN_SERVER_FETCH_RESULT_HTTP_SERVER_ERROR:
        return "http_server_error";
    case TOKEN_SERVER_FETCH_RESULT_INVALID_RESPONSE:
        return "invalid_response";
    default:
        return "unknown";
    }
}

static void init_token_server_fetch_diag(token_server_fetch_diag_t *diag)
{
    if (diag == NULL) {
        return;
    }
    memset(diag, 0, sizeof(*diag));
    diag->result = TOKEN_SERVER_FETCH_RESULT_TRANSPORT;
}

static void set_token_server_fetch_error_text(token_server_fetch_diag_t *diag, const char *text)
{
    if (diag == NULL || text == NULL || text[0] == '\0') {
        return;
    }

    strlcpy(diag->error_text, text, sizeof(diag->error_text));
}

static void token_server_show_retry_message(const char *line1, const char *line2)
{
    app_diagnostics_note_failure("token_retry", line1);
    lichuang_ui_show_message(
        "LIVEKIT",
        line1 != NULL ? line1 : "TOKEN RETRY",
        line2 != NULL ? line2 : "TRYING AGAIN",
        ":|");
}

static void token_server_show_auth_expired(void)
{
    runtime_clear_token_refresh_pending();
    app_diagnostics_note_failure("auth_expired", "token_server_auth_failures");
    lichuang_ui_show_message("LIVEKIT", "AUTH EXPIRED", "CONTACT ADMIN", ":(");
}

static bool token_server_failure_should_reconnect(livekit_failure_reason_t reason)
{
    switch (reason) {
    case LIVEKIT_FAILURE_REASON_NONE:
    case LIVEKIT_FAILURE_REASON_BAD_TOKEN:
    case LIVEKIT_FAILURE_REASON_UNAUTHORIZED:
    case LIVEKIT_FAILURE_REASON_DUPLICATE_IDENTITY:
    case LIVEKIT_FAILURE_REASON_PARTICIPANT_REMOVED:
    case LIVEKIT_FAILURE_REASON_ROOM_DELETED:
    case LIVEKIT_FAILURE_REASON_ROOM_CLOSED:
    case LIVEKIT_FAILURE_REASON_SIP_USER_UNAVAILABLE:
    case LIVEKIT_FAILURE_REASON_SIP_USER_REJECTED:
    case LIVEKIT_FAILURE_REASON_SIP_TRUNK_FAILURE:
        return false;
    default:
        return true;
    }
}

static bool token_server_auth_failure_should_retry(uint8_t failure_count)
{
    if (failure_count >= CONFIG_LK_EXAMPLE_TOKEN_SERVER_AUTH_MAX_FAILURES) {
        ESP_LOGE(TAG,
            "Token auth failed too many times: %u/%u",
            failure_count,
            (unsigned)CONFIG_LK_EXAMPLE_TOKEN_SERVER_AUTH_MAX_FAILURES);
        token_server_show_auth_expired();
        return false;
    }

    char detail[48];
    snprintf(detail, sizeof(detail), "RETRY %u/%u", failure_count, (unsigned)CONFIG_LK_EXAMPLE_TOKEN_SERVER_AUTH_MAX_FAILURES);
    token_server_show_retry_message("REFRESHING TOKEN", detail);
    return true;
}

static bool token_server_schedule_reconnect(livekit_failure_reason_t reason, uint32_t delay_ms)
{
    char detail[48];
    const char *reason_text = livekit_failure_reason_str(reason);

    if (!token_server_failure_should_reconnect(reason)) {
        return false;
    }

    if (!schedule_token_server_connect_task(delay_ms)) {
        ESP_LOGW(TAG,
            "Token reconnect task already scheduled or unavailable: reason=%s",
            reason_text);
        return true;
    }

    if (delay_ms == 0) {
        snprintf(detail, sizeof(detail), "%s NOW", reason_text);
    } else {
        snprintf(detail, sizeof(detail), "%s IN %lus",
            reason_text,
            (unsigned long)((delay_ms + 999U) / 1000U));
    }
    token_server_show_retry_message("RECONNECTING", detail);
    ESP_LOGW(TAG,
        "Scheduling token-server reconnect: reason=%s delay_ms=%" PRIu32,
        reason_text,
        delay_ms);
    return true;
}
#endif

#if CONFIG_LK_EXAMPLE_USE_DEVICE_JWT
static bool clock_is_valid_for_jwt(void)
{
    time_t now = 0;
    struct tm timeinfo = {0};

    time(&now);
    localtime_r(&now, &timeinfo);
    return timeinfo.tm_year >= (2024 - 1900);
}

static bool base64url_encode_alloc(const uint8_t *input, size_t input_len, char **out)
{
    size_t encoded_len = ((input_len + 2U) / 3U) * 4U;
    size_t actual_len = 0;
    int ret;
    char *buffer = NULL;

    if (out == NULL || input == NULL) {
        return false;
    }
    *out = NULL;

    buffer = (char *)calloc(1, encoded_len + 1U);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate base64 buffer");
        return false;
    }

    ret = mbedtls_base64_encode(
        (unsigned char *)buffer,
        encoded_len + 1U,
        &actual_len,
        input,
        input_len);
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_base64_encode failed: -0x%04x", -ret);
        free(buffer);
        return false;
    }
    buffer[actual_len] = '\0';

    size_t write_index = 0;
    for (size_t read_index = 0; read_index < actual_len; ++read_index) {
        char ch = buffer[read_index];
        if (ch == '=') {
            continue;
        }
        if (ch == '+') {
            ch = '-';
        } else if (ch == '/') {
            ch = '_';
        }
        buffer[write_index++] = ch;
    }
    buffer[write_index] = '\0';

    *out = buffer;
    return true;
}

static bool json_to_base64url_alloc(cJSON *json, char **out)
{
    char *json_text = NULL;
    bool ok = false;

    if (out == NULL || json == NULL) {
        return false;
    }
    *out = NULL;

    json_text = cJSON_PrintUnformatted(json);
    if (json_text == NULL) {
        ESP_LOGE(TAG, "Failed to serialize JWT JSON");
        return false;
    }

    ok = base64url_encode_alloc((const uint8_t *)json_text, strlen(json_text), out);
    cJSON_free(json_text);
    return ok;
}

static cJSON *build_device_jwt_payload(time_t now)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON *video = cJSON_CreateObject();
    cJSON *room_config = NULL;
    cJSON *agents = NULL;
    cJSON *agent = NULL;

    if (payload == NULL || video == NULL) {
        goto fail;
    }

    cJSON_AddStringToObject(payload, "iss", CONFIG_LK_EXAMPLE_API_KEY);
    cJSON_AddStringToObject(payload, "sub", active_participant_identity());
    cJSON_AddNumberToObject(payload, "nbf", (double)now);
    cJSON_AddNumberToObject(payload, "exp", (double)(now + CONFIG_LK_EXAMPLE_DEVICE_JWT_TTL_SECONDS));
    cJSON_AddStringToObject(payload, "name", CONFIG_LK_EXAMPLE_PARTICIPANT_NAME);

    cJSON_AddBoolToObject(video, "roomJoin", true);
    cJSON_AddStringToObject(video, "room", active_room_name());
    cJSON_AddBoolToObject(video, "canPublish", true);
    cJSON_AddBoolToObject(video, "canSubscribe", true);
    cJSON_AddBoolToObject(video, "canPublishData", LK_PUBLISHER_DATA_CHANNEL_ENABLED);
    cJSON_AddItemToObject(payload, "video", video);
    video = NULL;

    if (strlen(CONFIG_LK_EXAMPLE_PARTICIPANT_METADATA) > 0) {
        cJSON_AddStringToObject(payload, "metadata", CONFIG_LK_EXAMPLE_PARTICIPANT_METADATA);
    }

    if (strlen(CONFIG_LK_EXAMPLE_AGENT_NAME) > 0) {
        room_config = cJSON_CreateObject();
        agents = cJSON_CreateArray();
        agent = cJSON_CreateObject();
        if (room_config == NULL || agents == NULL || agent == NULL) {
            goto fail;
        }

        cJSON_AddStringToObject(agent, "agentName", CONFIG_LK_EXAMPLE_AGENT_NAME);
        if (strlen(CONFIG_LK_EXAMPLE_AGENT_METADATA) > 0) {
            cJSON_AddStringToObject(agent, "metadata", CONFIG_LK_EXAMPLE_AGENT_METADATA);
        }

        cJSON_AddItemToArray(agents, agent);
        agent = NULL;
        cJSON_AddItemToObject(room_config, "agents", agents);
        agents = NULL;
        cJSON_AddItemToObject(payload, "roomConfig", room_config);
        room_config = NULL;
    }

    return payload;

fail:
    if (agent != NULL) {
        cJSON_Delete(agent);
    }
    if (agents != NULL) {
        cJSON_Delete(agents);
    }
    if (room_config != NULL) {
        cJSON_Delete(room_config);
    }
    if (video != NULL) {
        cJSON_Delete(video);
    }
    if (payload != NULL) {
        cJSON_Delete(payload);
    }
    return NULL;
}

static bool sign_hs256_jwt_payload(cJSON *payload, char *token_out, size_t token_out_size)
{
    cJSON *header = NULL;
    char *header_b64 = NULL;
    char *payload_b64 = NULL;
    char *signing_input = NULL;
    char *signature_b64 = NULL;
    unsigned char digest[LIVEKIT_JWT_HMAC_SIZE];
    const mbedtls_md_info_t *md_info = NULL;
    time_t now = 0;
    bool ok = false;
    int ret;

    if (payload == NULL || token_out == NULL || token_out_size == 0) {
        return false;
    }

    if (!clock_is_valid_for_jwt()) {
        ESP_LOGE(TAG, "Device clock is not valid for JWT generation");
        return false;
    }

    time(&now);
    md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md_info == NULL) {
        ESP_LOGE(TAG, "Failed to load SHA256 md info");
        return false;
    }

    header = cJSON_CreateObject();
    if (header == NULL) {
        ESP_LOGE(TAG, "Failed to allocate JWT header JSON");
        goto cleanup;
    }
    cJSON_AddStringToObject(header, "alg", "HS256");
    cJSON_AddStringToObject(header, "typ", "JWT");

    if (!json_to_base64url_alloc(header, &header_b64) || !json_to_base64url_alloc(payload, &payload_b64)) {
        goto cleanup;
    }

    size_t signing_input_len = strlen(header_b64) + 1U + strlen(payload_b64);
    signing_input = (char *)malloc(signing_input_len + 1U);
    if (signing_input == NULL) {
        ESP_LOGE(TAG, "Failed to allocate JWT signing input");
        goto cleanup;
    }
    snprintf(signing_input, signing_input_len + 1U, "%s.%s", header_b64, payload_b64);

    ret = mbedtls_md_hmac(
        md_info,
        (const unsigned char *)CONFIG_LK_EXAMPLE_API_SECRET,
        strlen(CONFIG_LK_EXAMPLE_API_SECRET),
        (const unsigned char *)signing_input,
        strlen(signing_input),
        digest);
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_md_hmac failed: -0x%04x", -ret);
        goto cleanup;
    }

    if (!base64url_encode_alloc(digest, sizeof(digest), &signature_b64)) {
        goto cleanup;
    }

    int written = snprintf(
        token_out,
        token_out_size,
        "%s.%s",
        signing_input,
        signature_b64);
    if (written <= 0 || written >= (int)token_out_size) {
        ESP_LOGE(TAG, "JWT token exceeds buffer");
        goto cleanup;
    }
    ok = true;

cleanup:
    if (header != NULL) {
        cJSON_Delete(header);
    }
    free(header_b64);
    free(payload_b64);
    free(signing_input);
    free(signature_b64);
    return ok;
}

static bool build_device_jwt_credentials(fetched_room_credentials_t *credentials)
{
    cJSON *payload = NULL;
    time_t now = 0;
    bool ok = false;

    if (credentials == NULL) {
        return false;
    }
    memset(credentials, 0, sizeof(*credentials));

    time(&now);
    payload = build_device_jwt_payload(now);
    if (payload == NULL) {
        ESP_LOGE(TAG, "Failed to build JWT payload");
        goto cleanup;
    }

    if (!sign_hs256_jwt_payload(payload, credentials->token, sizeof(credentials->token))) {
        goto cleanup;
    }

    strlcpy(credentials->server_url, CONFIG_LK_EXAMPLE_SERVER_URL, sizeof(credentials->server_url));
    if (credentials->server_url[0] == '\0') {
        ESP_LOGE(TAG, "LiveKit server URL is empty");
        goto cleanup;
    }

    ESP_LOGI(
        TAG,
        "Generated device JWT room=%s identity=%s exp=%" PRIu64,
        active_room_name(),
        active_participant_identity(),
        (uint64_t)(now + CONFIG_LK_EXAMPLE_DEVICE_JWT_TTL_SECONDS));
    ok = true;

cleanup:
    if (payload != NULL) {
        cJSON_Delete(payload);
    }
    return ok;
}

static cJSON *build_room_admin_jwt_payload(time_t now, const char *room_name)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON *video = cJSON_CreateObject();
    char subject[96];

    if (payload == NULL || video == NULL) {
        goto fail;
    }

    snprintf(subject, sizeof(subject), "%s-dispatch", active_participant_identity());

    cJSON_AddStringToObject(payload, "iss", CONFIG_LK_EXAMPLE_API_KEY);
    cJSON_AddStringToObject(payload, "sub", subject);
    cJSON_AddNumberToObject(payload, "nbf", (double)now);
    cJSON_AddNumberToObject(payload, "exp", (double)(now + CONFIG_LK_EXAMPLE_DEVICE_JWT_TTL_SECONDS));
    cJSON_AddStringToObject(payload, "name", subject);

    cJSON_AddBoolToObject(video, "roomAdmin", true);
    cJSON_AddStringToObject(video, "room", room_name);
    cJSON_AddItemToObject(payload, "video", video);
    return payload;

fail:
    if (video != NULL) {
        cJSON_Delete(video);
    }
    if (payload != NULL) {
        cJSON_Delete(payload);
    }
    return NULL;
}

static bool build_room_admin_jwt_token(const char *room_name, char *token_out, size_t token_out_size)
{
    cJSON *payload = NULL;
    time_t now = 0;
    bool ok = false;

    if (room_name == NULL || room_name[0] == '\0') {
        return false;
    }

    time(&now);
    payload = build_room_admin_jwt_payload(now, room_name);
    if (payload == NULL) {
        ESP_LOGE(TAG, "Failed to build room admin JWT payload");
        return false;
    }

    ok = sign_hs256_jwt_payload(payload, token_out, token_out_size);
    cJSON_Delete(payload);
    return ok;
}

static bool build_agent_dispatch_url(char *out, size_t out_size)
{
    const char *server_url = CONFIG_LK_EXAMPLE_SERVER_URL;
    const char *remainder = server_url;
    const char *scheme = NULL;

    if (out == NULL || out_size == 0 || server_url[0] == '\0') {
        return false;
    }

    if (strncmp(server_url, "wss://", 6) == 0) {
        scheme = "https://";
        remainder = server_url + 6;
    } else if (strncmp(server_url, "ws://", 5) == 0) {
        scheme = "http://";
        remainder = server_url + 5;
    } else if (strncmp(server_url, "https://", 8) == 0) {
        scheme = "";
    } else if (strncmp(server_url, "http://", 7) == 0) {
        scheme = "";
    } else {
        ESP_LOGE(TAG, "Unsupported LiveKit server URL scheme: %s", server_url);
        return false;
    }

    int written = snprintf(out, out_size, "%s%s", scheme, remainder);
    if (written <= 0 || written >= (int)out_size) {
        ESP_LOGE(TAG, "Agent dispatch URL exceeds buffer");
        return false;
    }

    size_t len = strlen(out);
    while (len > 0 && out[len - 1] == '/') {
        out[--len] = '\0';
    }

    return strlcat(out, "/twirp/livekit.AgentDispatchService/CreateDispatch", out_size) < out_size;
}

static bool build_agent_dispatch_request_body(
    const char *room_name,
    const char *agent_name,
    char **out_body)
{
    cJSON *root = NULL;
    char *body = NULL;

    if (room_name == NULL || room_name[0] == '\0' ||
        agent_name == NULL || agent_name[0] == '\0' ||
        out_body == NULL) {
        return false;
    }

    *out_body = NULL;
    root = cJSON_CreateObject();
    if (root == NULL) {
        return false;
    }

    cJSON_AddStringToObject(root, "room", room_name);
    cJSON_AddStringToObject(root, "agent_name", agent_name);
    if (strlen(CONFIG_LK_EXAMPLE_AGENT_METADATA) > 0) {
        cJSON_AddStringToObject(root, "metadata", CONFIG_LK_EXAMPLE_AGENT_METADATA);
    }

    body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        return false;
    }

    *out_body = body;
    return true;
}

static void agent_dispatch_task(void *arg)
{
    agent_dispatch_task_args_t *task_args = (agent_dispatch_task_args_t *)arg;
    esp_http_client_handle_t client = NULL;
    http_response_buffer_t *response = NULL;
    char *admin_token = NULL;
    char *authorization_header = NULL;
    char *body = NULL;
    bool request_ok = false;

    if (task_args == NULL) {
        s_runtime.agent_dispatch_inflight = false;
        vTaskDeleteWithCaps(NULL);
        return;
    }

    if (!active_room_sid_matches(task_args->room_sid)) {
        ESP_LOGI(TAG, "Skip stale agent dispatch request for room sid=%s", task_args->room_sid);
        goto cleanup;
    }

    response = (http_response_buffer_t *)alloc_preferred_zeroed(sizeof(*response));
    admin_token = (char *)alloc_preferred_zeroed(TOKEN_SERVER_TOKEN_MAX);
    authorization_header = (char *)alloc_preferred_zeroed(TOKEN_SERVER_TOKEN_MAX + 16);
    if (response == NULL || admin_token == NULL || authorization_header == NULL) {
        ESP_LOGE(TAG, "Failed to allocate buffers for agent dispatch");
        goto cleanup;
    }

    ESP_LOGI(TAG,
        "Agent dispatch start: room=%s agent=%s stack_hwm=%" PRIu32,
        task_args->room_name,
        task_args->agent_name,
        (uint32_t)uxTaskGetStackHighWaterMark(NULL));

    if (!build_room_admin_jwt_token(task_args->room_name, admin_token, TOKEN_SERVER_TOKEN_MAX)) {
        ESP_LOGE(TAG, "Failed to build room admin JWT for agent dispatch");
        goto cleanup;
    }

    if (!build_agent_dispatch_request_body(task_args->room_name, task_args->agent_name, &body)) {
        ESP_LOGE(TAG, "Failed to build agent dispatch request body");
        goto cleanup;
    }

    esp_http_client_config_t config = {
        .url = task_args->url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .event_handler = http_response_buffer_event_handler,
        .user_data = response,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to create agent dispatch HTTP client");
        goto cleanup;
    }

    snprintf(authorization_header, TOKEN_SERVER_TOKEN_MAX + 16, "Bearer %s", admin_token);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "Authorization", authorization_header));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "Content-Type", "application/json"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "Accept", "application/json"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_post_field(client, body, (int)strlen(body)));

    ESP_LOGI(
        TAG,
        "Requesting agent dispatch: room=%s agent=%s url=%s",
        task_args->room_name,
        task_args->agent_name,
        task_args->url);

    if (esp_http_client_perform(client) != ESP_OK) {
        ESP_LOGE(TAG, "Agent dispatch HTTP request failed");
        goto cleanup;
    }

    int status = esp_http_client_get_status_code(client);
    if (status >= 200 && status < 300) {
        ESP_LOGI(TAG, "Agent dispatch created successfully: %s", response->body);
        request_ok = true;
    } else if (status == 409 || strstr(response->body, "\"code\":\"already_exists\"") != NULL) {
        ESP_LOGW(TAG, "Agent dispatch already exists: %s", response->body);
        request_ok = true;
    } else {
        ESP_LOGE(TAG, "Agent dispatch failed: HTTP %d body=%s", status, response->body);
    }

cleanup:
    if (client != NULL) {
        esp_http_client_cleanup(client);
    }
    cJSON_free(body);
    free(authorization_header);
    free(admin_token);
    free(response);
    if (request_ok && active_room_sid_matches(task_args != NULL ? task_args->room_sid : NULL)) {
        s_runtime.agent_dispatch_requested = true;
    }
    s_runtime.agent_dispatch_inflight = false;
    free(task_args);
    vTaskDeleteWithCaps(NULL);
}

static void delayed_agent_dispatch_task(void *arg)
{
    char room_sid[32] = {0};

    if (arg != NULL) {
        strlcpy(room_sid, (const char *)arg, sizeof(room_sid));
        free(arg);
    }

    ESP_LOGI(TAG, "Agent dispatch delayed by %d ms for room sid=%s", AGENT_DISPATCH_DELAY_MS, room_sid);
    log_heap_state("dispatch-delay-start");
    vTaskDelay(pdMS_TO_TICKS(AGENT_DISPATCH_DELAY_MS));

    s_runtime.agent_dispatch_scheduled = false;
    if (!active_room_sid_matches(room_sid) || s_runtime.agent_active) {
        ESP_LOGI(TAG, "Skip delayed agent dispatch for stale/inactive room sid=%s", room_sid);
        vTaskDeleteWithCaps(NULL);
        return;
    }

    log_heap_state("dispatch-delay-fire");
    request_agent_dispatch_if_needed();
    vTaskDeleteWithCaps(NULL);
}

static void request_agent_dispatch_if_needed(void)
{
    agent_dispatch_task_args_t *task_args = NULL;

    if (strlen(CONFIG_LK_EXAMPLE_AGENT_NAME) == 0) {
        return;
    }
    if (s_runtime.agent_active ||
        s_runtime.agent_dispatch_scheduled ||
        s_runtime.agent_dispatch_requested ||
        s_runtime.agent_dispatch_inflight) {
        return;
    }
    if (s_runtime.room_sid[0] == '\0') {
        return;
    }

    task_args = (agent_dispatch_task_args_t *)alloc_internal_zeroed(sizeof(*task_args));
    if (task_args == NULL) {
        ESP_LOGE(TAG, "Failed to allocate agent dispatch task args");
        return;
    }

    if (!build_agent_dispatch_url(task_args->url, sizeof(task_args->url))) {
        free(task_args);
        return;
    }

    strlcpy(task_args->room_name, active_room_name(), sizeof(task_args->room_name));
    strlcpy(task_args->room_sid, s_runtime.room_sid, sizeof(task_args->room_sid));
    strlcpy(task_args->agent_name, CONFIG_LK_EXAMPLE_AGENT_NAME, sizeof(task_args->agent_name));

    s_runtime.agent_dispatch_inflight = true;
    BaseType_t task_ok = xTaskCreateWithCaps(
        agent_dispatch_task,
        "lk_dispatch",
        AGENT_DISPATCH_TASK_STACK_SIZE,
        task_args,
        AGENT_DISPATCH_TASK_PRIORITY,
        NULL,
        MALLOC_CAP_SPIRAM);
    if (task_ok != pdPASS) {
        s_runtime.agent_dispatch_inflight = false;
        ESP_LOGE(TAG, "Failed to start agent dispatch task");
        free(task_args);
    }
}

static void schedule_agent_dispatch_if_needed(void)
{
    char *room_sid = NULL;
    BaseType_t task_ok;

    if (strlen(CONFIG_LK_EXAMPLE_AGENT_NAME) == 0) {
        return;
    }
    if (s_runtime.agent_active ||
        s_runtime.agent_dispatch_scheduled ||
        s_runtime.agent_dispatch_requested ||
        s_runtime.agent_dispatch_inflight) {
        return;
    }
    if (s_runtime.room_sid[0] == '\0') {
        return;
    }

    room_sid = (char *)alloc_internal_zeroed(sizeof(s_runtime.room_sid));
    if (room_sid == NULL) {
        ESP_LOGE(TAG, "Failed to allocate delayed agent dispatch room sid");
        return;
    }
    strlcpy(room_sid, s_runtime.room_sid, sizeof(s_runtime.room_sid));

    s_runtime.agent_dispatch_scheduled = true;
    task_ok = xTaskCreateWithCaps(
        delayed_agent_dispatch_task,
        "lk_dispatch_wait",
        AGENT_DISPATCH_DELAY_TASK_STACK_SIZE,
        room_sid,
        AGENT_DISPATCH_DELAY_TASK_PRIORITY,
        NULL,
        MALLOC_CAP_SPIRAM);
    if (task_ok != pdPASS) {
        s_runtime.agent_dispatch_scheduled = false;
        ESP_LOGE(TAG, "Failed to start delayed agent dispatch task");
        free(room_sid);
        return;
    }

    ESP_LOGI(TAG, "Scheduled delayed agent dispatch for room sid=%s", s_runtime.room_sid);
}
#else
static void schedule_agent_dispatch_if_needed(void)
{
}
#endif

#if CONFIG_LK_EXAMPLE_USE_TOKEN_SERVER
static bool build_token_server_request_body(char **out_body)
{
    cJSON *root = NULL;
    char *body = NULL;

    if (out_body == NULL) {
        return false;
    }
    *out_body = NULL;

    root = cJSON_CreateObject();
    if (root == NULL) {
        return false;
    }

    cJSON_AddStringToObject(root, "room_name", active_room_name());
    cJSON_AddStringToObject(root, "participant_identity", active_participant_identity());
    cJSON_AddStringToObject(root, "participant_name", CONFIG_LK_EXAMPLE_PARTICIPANT_NAME);
    cJSON_AddStringToObject(root, "device_model", "lichuang_esp32s3");

    if (strlen(CONFIG_LK_EXAMPLE_PARTICIPANT_METADATA) > 0) {
        cJSON_AddStringToObject(root, "participant_metadata", CONFIG_LK_EXAMPLE_PARTICIPANT_METADATA);
    }
    if (strlen(CONFIG_LK_EXAMPLE_AGENT_NAME) > 0) {
        cJSON_AddStringToObject(root, "agent_name", CONFIG_LK_EXAMPLE_AGENT_NAME);
    }
    if (strlen(CONFIG_LK_EXAMPLE_AGENT_METADATA) > 0) {
        cJSON_AddStringToObject(root, "agent_metadata", CONFIG_LK_EXAMPLE_AGENT_METADATA);
    }

    body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        return false;
    }

    *out_body = body;
    return true;
}

static bool parse_token_server_response(
    const char *body,
    fetched_room_credentials_t *credentials,
    token_server_fetch_diag_t *diag)
{
    cJSON *root = NULL;
    cJSON *server_url = NULL;
    cJSON *token = NULL;
    bool ok = false;

    if (body == NULL || credentials == NULL) {
        return false;
    }

    root = cJSON_Parse(body);
    if (root == NULL) {
        ESP_LOGE(TAG, "Token server returned invalid JSON");
        if (diag != NULL) {
            diag->result = TOKEN_SERVER_FETCH_RESULT_INVALID_RESPONSE;
            set_token_server_fetch_error_text(diag, "invalid_json");
        }
        goto cleanup;
    }

    server_url = cJSON_GetObjectItemCaseSensitive(root, "server_url");
    token = cJSON_GetObjectItemCaseSensitive(root, "token");
    if (!cJSON_IsString(server_url) || server_url->valuestring == NULL ||
        !cJSON_IsString(token) || token->valuestring == NULL) {
        ESP_LOGE(TAG, "Token server response missing server_url or token");
        if (diag != NULL) {
            diag->result = TOKEN_SERVER_FETCH_RESULT_INVALID_RESPONSE;
            set_token_server_fetch_error_text(diag, "missing_server_url_or_token");
        }
        goto cleanup;
    }

    strlcpy(credentials->server_url, server_url->valuestring, sizeof(credentials->server_url));
    strlcpy(credentials->token, token->valuestring, sizeof(credentials->token));
    ok = credentials->server_url[0] != '\0' && credentials->token[0] != '\0';
    if (ok && diag != NULL) {
        diag->result = TOKEN_SERVER_FETCH_RESULT_OK;
    }

cleanup:
    if (root != NULL) {
        cJSON_Delete(root);
    }
    return ok;
}

static bool fetch_token_server_credentials(
    fetched_room_credentials_t *credentials,
    token_server_fetch_diag_t *diag)
{
    esp_http_client_handle_t client = NULL;
    char *body = NULL;
    http_response_buffer_t *response = NULL;
    bool ok = false;

    if (credentials == NULL) {
        return false;
    }
    memset(credentials, 0, sizeof(*credentials));
    init_token_server_fetch_diag(diag);

    response = (http_response_buffer_t *)alloc_preferred_zeroed(sizeof(*response));
    if (response == NULL) {
        ESP_LOGE(TAG, "Failed to allocate token server response buffer");
        if (diag != NULL) {
            diag->result = TOKEN_SERVER_FETCH_RESULT_TRANSPORT;
            set_token_server_fetch_error_text(diag, "response_buffer_alloc_failed");
        }
        return false;
    }

    if (!build_token_server_request_body(&body)) {
        ESP_LOGE(TAG, "Failed to build token server request body");
        if (diag != NULL) {
            diag->result = TOKEN_SERVER_FETCH_RESULT_INVALID_RESPONSE;
            set_token_server_fetch_error_text(diag, "request_body_failed");
        }
        return false;
    }

    esp_http_client_config_t config = {
        .url = CONFIG_LK_EXAMPLE_TOKEN_SERVER_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CONFIG_LK_EXAMPLE_TOKEN_SERVER_TIMEOUT_MS,
        .event_handler = http_response_buffer_event_handler,
        .user_data = response,
    };
    client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init token server HTTP client");
        if (diag != NULL) {
            diag->result = TOKEN_SERVER_FETCH_RESULT_TRANSPORT;
            set_token_server_fetch_error_text(diag, "http_client_init_failed");
        }
        goto cleanup;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "Content-Type", "application/json"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_header(client, "Accept", "application/json"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_http_client_set_post_field(client, body, (int)strlen(body)));

    ESP_LOGI(TAG, "Requesting token from %s", CONFIG_LK_EXAMPLE_TOKEN_SERVER_URL);
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Token server request failed: %s", esp_err_to_name(err));
        if (diag != NULL) {
            diag->result = TOKEN_SERVER_FETCH_RESULT_TRANSPORT;
            set_token_server_fetch_error_text(diag, esp_err_to_name(err));
        }
        goto cleanup;
    }

    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "Token server returned HTTP %d body=%s", status, response->body);
        if (diag != NULL) {
            diag->http_status = status;
            if (status == 401 || status == 403) {
                diag->result = TOKEN_SERVER_FETCH_RESULT_HTTP_UNAUTHORIZED;
            } else if (status >= 400 && status < 500) {
                diag->result = TOKEN_SERVER_FETCH_RESULT_HTTP_CLIENT_ERROR;
            } else {
                diag->result = TOKEN_SERVER_FETCH_RESULT_HTTP_SERVER_ERROR;
            }
            set_token_server_fetch_error_text(diag, response->body);
        }
        goto cleanup;
    }

    ok = parse_token_server_response(response->body, credentials, diag);

cleanup:
    if (client != NULL) {
        esp_http_client_cleanup(client);
    }
    if (body != NULL) {
        cJSON_free(body);
    }
    if (response != NULL) {
        free(response);
    }
    return ok;
}
#endif

static bool create_room_handle(void)
{
    if (s_room_handle != NULL) {
        ESP_LOGW(TAG, "Destroying stale room before create");
        destroy_room();
    }

    livekit_room_options_t room_options = {
        .publish = {
            .kind = LIVEKIT_MEDIA_TYPE_AUDIO,
            .audio_encode = {
                .codec = LIVEKIT_AUDIO_CODEC_OPUS,
                .sample_rate = 16000,
                .channel_count = 1,
            },
            .capturer = livekit_media_get_capturer(),
        },
        .subscribe = {
            .kind = LIVEKIT_MEDIA_TYPE_AUDIO,
            .renderer = livekit_media_get_renderer(),
        },
        .on_state_changed = on_state_changed,
        .on_data_received = on_data_received,
        .on_room_info = on_room_info,
        .on_participant_info = on_participant_info,
    };

    if (livekit_room_create(&s_room_handle, &room_options) != LIVEKIT_ERR_NONE) {
        app_diagnostics_note_failure("room_create_failed", "livekit_room_create");
        ESP_LOGE(TAG, "Failed to create room");
        lichuang_ui_show_message("LIVEKIT", "ROOM CREATE FAILED", "CHECK SERIAL LOG", ":(");
        return false;
    }

    return true;
}

#if CONFIG_LK_EXAMPLE_USE_TOKEN_SERVER
static void token_server_connect_task(void *arg)
{
    token_server_connect_task_args_t *task_args = (token_server_connect_task_args_t *)arg;
    uint32_t generation = task_args != NULL ? task_args->generation : 0;
    uint32_t delay_ms = task_args != NULL ? task_args->delay_ms : 0;
    fetched_room_credentials_t credentials = {};
    token_server_fetch_diag_t diag = {};

    log_heap_state("token-task-start");
    ESP_LOGI(TAG,
        "Token connect task start: generation=%" PRIu32 " delay_ms=%" PRIu32 " stack_hwm=%" PRIu32,
        generation,
        delay_ms,
        (uint32_t)uxTaskGetStackHighWaterMark(NULL));
    while (!runtime_is_stop_requested()) {
        if (!runtime_is_current_generation(generation)) {
            ESP_LOGI(TAG, "Skip stale token connect task generation=%" PRIu32, generation);
            break;
        }

        if (delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            delay_ms = 0;
            if (runtime_is_stop_requested() || !runtime_is_current_generation(generation)) {
                break;
            }
        }

        reset_connection_runtime_state(false);
        app_diagnostics_note_stage("token_fetch");
        destroy_room();
        log_token_server_diagnostics(CONFIG_LK_EXAMPLE_TOKEN_SERVER_URL);

        if (!fetch_token_server_credentials(&credentials, &diag)) {
            app_diagnostics_note_failure("token_fetch_failed", token_server_fetch_result_str(diag.result));
            ESP_LOGW(TAG,
                "Token fetch failed: result=%s http_status=%d error=%s",
                token_server_fetch_result_str(diag.result),
                diag.http_status,
                diag.error_text[0] != '\0' ? diag.error_text : "(none)");

            if (diag.result == TOKEN_SERVER_FETCH_RESULT_HTTP_UNAUTHORIZED) {
                uint8_t failure_count = runtime_record_auth_failure();
                if (!token_server_auth_failure_should_retry(failure_count)) {
                    break;
                }
            } else {
                token_server_show_retry_message("TOKEN SERVER", "RETRYING SOON");
            }

            delay_ms = CONFIG_LK_EXAMPLE_TOKEN_SERVER_RETRY_DELAY_MS;
            continue;
        }

        if (!create_room_handle()) {
            app_diagnostics_note_failure("room_create_failed", "token_supervisor");
            token_server_show_retry_message("ROOM CREATE", "RETRYING SOON");
            delay_ms = CONFIG_LK_EXAMPLE_TOKEN_SERVER_RETRY_DELAY_MS;
            continue;
        }

        log_livekit_network_diagnostics(credentials.server_url);
        app_diagnostics_note_stage("room_connect_call");
        livekit_err_t connect_res = livekit_room_connect(s_room_handle, credentials.server_url, credentials.token);
        if (connect_res != LIVEKIT_ERR_NONE) {
            app_diagnostics_note_failure("room_connect_failed", "livekit_room_connect");
            ESP_LOGE(TAG, "Failed to connect to room with refreshed token");
            destroy_room();
            token_server_show_retry_message("CONNECT FAILED", "RETRYING SOON");
            delay_ms = CONFIG_LK_EXAMPLE_TOKEN_SERVER_RETRY_DELAY_MS;
            continue;
        }

        ESP_LOGI(TAG, "Token-server connect attempt started");
        runtime_mark_token_refresh_pending();
        break;
    }

    runtime_finish_token_connect_task();
    free(task_args);
    vTaskDeleteWithCaps(NULL);
}

static bool schedule_token_server_connect_task(uint32_t delay_ms)
{
    token_server_connect_task_args_t *task_args = NULL;
    BaseType_t task_ok;
    uint32_t generation = 0;

    if (!runtime_begin_token_connect_task(&generation)) {
        return false;
    }

    task_args = (token_server_connect_task_args_t *)alloc_internal_zeroed(sizeof(*task_args));
    if (task_args == NULL) {
        runtime_finish_token_connect_task();
        ESP_LOGE(TAG, "Failed to allocate token connect task args");
        return false;
    }

    task_args->generation = generation;
    task_args->delay_ms = delay_ms;

    task_ok = xTaskCreateWithCaps(
        token_server_connect_task,
        "lk_token_conn",
        TOKEN_SERVER_CONNECT_TASK_STACK_SIZE,
        task_args,
        TOKEN_SERVER_CONNECT_TASK_PRIORITY,
        NULL,
        MALLOC_CAP_SPIRAM);
    if (task_ok != pdPASS) {
        runtime_finish_token_connect_task();
        free(task_args);
        ESP_LOGE(TAG, "Failed to start token connect task");
        return false;
    }

    return true;
}
#endif

static void destroy_room(void)
{
    if (s_room_handle == NULL) {
        return;
    }
    livekit_room_destroy(s_room_handle);
    s_room_handle = NULL;
}

static bool have_credentials(void)
{
#if CONFIG_LK_EXAMPLE_USE_SANDBOX
    if (strlen(CONFIG_LK_EXAMPLE_SANDBOX_ID) == 0) {
        ESP_LOGE(TAG, "Missing CONFIG_LK_EXAMPLE_SANDBOX_ID");
        return false;
    }
    return true;
#elif CONFIG_LK_EXAMPLE_USE_DEVICE_JWT
    if (strlen(CONFIG_LK_EXAMPLE_SERVER_URL) == 0 ||
        strlen(CONFIG_LK_EXAMPLE_API_KEY) == 0 ||
        strlen(CONFIG_LK_EXAMPLE_API_SECRET) == 0) {
        ESP_LOGE(TAG, "Missing CONFIG_LK_EXAMPLE_SERVER_URL, API_KEY, or API_SECRET");
        return false;
    }
    return true;
#elif CONFIG_LK_EXAMPLE_USE_TOKEN_SERVER
    if (strlen(CONFIG_LK_EXAMPLE_TOKEN_SERVER_URL) == 0) {
        ESP_LOGE(TAG, "Missing CONFIG_LK_EXAMPLE_TOKEN_SERVER_URL");
        return false;
    }
    return true;
#elif CONFIG_LK_EXAMPLE_USE_PREGENERATED
    if (strlen(CONFIG_LK_EXAMPLE_SERVER_URL) == 0 || strlen(CONFIG_LK_EXAMPLE_TOKEN) == 0) {
        ESP_LOGE(TAG, "Missing CONFIG_LK_EXAMPLE_SERVER_URL or CONFIG_LK_EXAMPLE_TOKEN");
        return false;
    }
    return true;
#else
    ESP_LOGE(TAG, "No credential mode selected");
    return false;
#endif
}

#if CONFIG_LK_EXAMPLE_USE_TOKEN_SERVER
static bool handle_token_server_room_auth_failure(livekit_failure_reason_t reason)
{
    if (reason != LIVEKIT_FAILURE_REASON_BAD_TOKEN &&
        reason != LIVEKIT_FAILURE_REASON_UNAUTHORIZED) {
        return false;
    }

    uint8_t failure_count = runtime_record_auth_failure();
    if (!token_server_auth_failure_should_retry(failure_count)) {
        return true;
    }

    if (!schedule_token_server_connect_task(CONFIG_LK_EXAMPLE_TOKEN_SERVER_RETRY_DELAY_MS)) {
        ESP_LOGW(TAG, "Token refresh task already scheduled or unavailable");
    }
    return true;
}
#endif

static void on_state_changed(livekit_connection_state_t state, void *ctx)
{
    (void)ctx;
    char detail[48];

    ESP_LOGI(TAG, "Room state changed: %s", livekit_connection_state_str(state));

    livekit_failure_reason_t reason = livekit_room_get_failure_reason(s_room_handle);
    if (reason != LIVEKIT_FAILURE_REASON_NONE) {
        ESP_LOGE(TAG, "Failure reason: %s", livekit_failure_reason_str(reason));
    }

    switch (state) {
    case LIVEKIT_CONNECTION_STATE_CONNECTING:
        app_diagnostics_note_stage("room_connecting");
        lichuang_ui_show_message("LIVEKIT", "CONNECTING", active_room_name(), ":)");
        break;
    case LIVEKIT_CONNECTION_STATE_RECONNECTING:
        app_diagnostics_note_failure("room_reconnecting", livekit_failure_reason_str(reason));
        lichuang_ui_show_message("LIVEKIT", "RECONNECTING", active_room_name(), ":|");
        break;
    case LIVEKIT_CONNECTION_STATE_CONNECTED:
        app_diagnostics_mark_boot_stable("room_connected");
        log_heap_state("room-connected");
        runtime_clear_auth_failure_state();
        ESP_ERROR_CHECK_WITHOUT_ABORT(lichuang_ui_resume());
        schedule_agent_dispatch_if_needed();
        if (have_chat_history()) {
            refresh_chat_ui();
        } else {
            snprintf(detail, sizeof(detail), "ROOM %.42s", active_room_name());
            lichuang_ui_show_online("VOICE READY", "AI", "Ready to talk");
        }
        break;
    case LIVEKIT_CONNECTION_STATE_DISCONNECTED:
        app_diagnostics_note_failure("room_disconnected", livekit_failure_reason_str(reason));
        ESP_ERROR_CHECK_WITHOUT_ABORT(lichuang_ui_resume());
        if (runtime_token_refresh_pending()) {
            lichuang_ui_show_message("LIVEKIT", "REFRESHING TOKEN", active_room_name(), ":|");
            break;
        }
        lichuang_ui_show_message("LIVEKIT", "DISCONNECTED", "PRESS RESET", ":(");
        break;
    case LIVEKIT_CONNECTION_STATE_FAILED:
        app_diagnostics_note_failure("room_failed", livekit_failure_reason_str(reason));
        ESP_ERROR_CHECK_WITHOUT_ABORT(lichuang_ui_resume());
#if CONFIG_LK_EXAMPLE_USE_TOKEN_SERVER
        if (handle_token_server_room_auth_failure(reason)) {
            break;
        }
        if (token_server_schedule_reconnect(reason, CONFIG_LK_EXAMPLE_TOKEN_SERVER_RETRY_DELAY_MS)) {
            break;
        }
        runtime_clear_token_refresh_pending();
#endif
        snprintf(detail, sizeof(detail), "FAIL %s", livekit_failure_reason_str(reason));
        lichuang_ui_show_message("LIVEKIT", "CONNECTION FAILED", detail, ":(");
        break;
    }
}

static void on_room_info(const livekit_room_info_t *info, void *ctx)
{
    (void)ctx;

    if (info == NULL) {
        return;
    }

    strlcpy(
        s_runtime.room_name,
        (info->name != NULL && info->name[0] != '\0') ? info->name : active_room_name(),
        sizeof(s_runtime.room_name));
    if (info->sid != NULL && info->sid[0] != '\0' && strcmp(s_runtime.room_sid, info->sid) != 0) {
        strlcpy(s_runtime.room_sid, info->sid, sizeof(s_runtime.room_sid));
        s_runtime.agent_dispatch_scheduled = false;
        s_runtime.agent_dispatch_requested = false;
        if (s_room_handle != NULL &&
            livekit_room_get_state(s_room_handle) == LIVEKIT_CONNECTION_STATE_CONNECTED) {
            ESP_LOGI(TAG, "Room sid became available after connect; scheduling agent dispatch");
            schedule_agent_dispatch_if_needed();
        }
    }

    ESP_LOGI(TAG,
        "Room info: sid=%s name=%s participants=%" PRIu32 " recording=%d",
        info->sid != NULL ? info->sid : "(none)",
        s_runtime.room_name,
        info->participant_count,
        info->active_recording);
}

static void on_participant_info(const livekit_participant_info_t *info, void *ctx)
{
    (void)ctx;

    if (info == NULL) {
        return;
    }

    ESP_LOGI(TAG,
        "Participant info: kind=%s state=%s identity=%s name=%s",
        participant_kind_str(info->kind),
        participant_state_str(info->state),
        info->identity != NULL ? info->identity : "(none)",
        info->name != NULL ? info->name : "(none)");

    if (info->kind != LIVEKIT_PARTICIPANT_KIND_AGENT) {
        return;
    }

    if (info->identity != NULL && info->identity[0] != '\0') {
        strlcpy(s_runtime.agent_identity, info->identity, sizeof(s_runtime.agent_identity));
    }
    if (info->name != NULL && info->name[0] != '\0') {
        strlcpy(s_runtime.agent_name, info->name, sizeof(s_runtime.agent_name));
    }

    char agent_label[48];
    format_agent_label(agent_label, sizeof(agent_label));

    switch (info->state) {
    case LIVEKIT_PARTICIPANT_STATE_ACTIVE:
        s_runtime.agent_active = true;
        s_runtime.agent_dispatch_requested = true;
        if (have_chat_history()) {
            refresh_chat_ui();
        } else {
            lichuang_ui_show_online("AI READY", agent_label, "Start speaking");
        }
        break;
    case LIVEKIT_PARTICIPANT_STATE_JOINED:
    case LIVEKIT_PARTICIPANT_STATE_JOINING:
        s_runtime.agent_active = false;
        lichuang_ui_show_online("AI JOINING", agent_label, "Negotiating media");
        break;
    case LIVEKIT_PARTICIPANT_STATE_DISCONNECTED:
        s_runtime.agent_active = false;
        lichuang_ui_show_message("AI LEFT", agent_label, "Waiting to rejoin", ":(");
        break;
    default:
        break;
    }
}

static void on_data_received(const livekit_data_received_t *data, void *ctx)
{
    (void)ctx;

    if (data == NULL) {
        return;
    }

    char payload_text[384];
    char preview[96];
    char headline[48];
    const char *topic = (data->topic != NULL && data->topic[0] != '\0') ? data->topic : "data";
    const char *sender = (data->sender_identity != NULL && data->sender_identity[0] != '\0')
        ? data->sender_identity
        : "remote";
    bool is_transcription = strcmp(topic, "lk.transcription") == 0;
    bool internal_topic = is_internal_topic(topic);
    bool is_text = is_transcription || payload_is_text(data->payload.bytes, data->payload.size);

    if (is_text) {
        normalize_payload_text(payload_text, sizeof(payload_text), data->payload.bytes, data->payload.size);
        format_payload_preview(preview, sizeof(preview), data->payload.bytes, data->payload.size);
    } else {
        payload_text[0] = '\0';
        snprintf(preview, sizeof(preview), "BINARY PAYLOAD %u BYTES", (unsigned)data->payload.size);
    }

    snprintf(headline, sizeof(headline), "%s / %s", sender, topic);
    s_runtime.last_text_us = esp_timer_get_time();

    ESP_LOGI(TAG,
        "Data received: sender=%s topic=%s size=%u preview=%s",
        sender,
        topic,
        (unsigned)data->payload.size,
        preview);

    if (is_transcription && payload_text[0] != '\0') {
        if (sender_is_agent(sender)) {
            update_assistant_text(payload_text);
        } else {
            update_user_text(payload_text);
        }
        return;
    }

    if (is_text && handle_json_payload(payload_text)) {
        return;
    }

    if (strcmp(topic, "lk.agent.session") == 0) {
        if (sender_is_agent(sender) &&
            extract_agent_session_text(payload_text, sizeof(payload_text), data->payload.bytes, data->payload.size)) {
            ESP_LOGI(TAG, "Session text extracted: %s", payload_text);
        }
        return;
    }

    if (internal_topic) {
        return;
    }

    lichuang_ui_show_online("DOWNLINK DATA", headline, preview);
}

void livekit_app_notify_remote_audio_info(const char *codec_name, uint32_t sample_rate, uint8_t channels)
{
    char agent_label[48];
    char detail[64];
    const char *codec = (codec_name != NULL && codec_name[0] != '\0') ? codec_name : "PCM";

    if (have_chat_history() || (s_runtime.last_text_us != 0 && (esp_timer_get_time() - s_runtime.last_text_us) < 4000000)) {
        return;
    }

    format_agent_label(agent_label, sizeof(agent_label));
    snprintf(detail, sizeof(detail), "%s  %" PRIu32 "HZ  %" PRIu8 "CH", codec, sample_rate, channels);
    lichuang_ui_show_online("REMOTE AUDIO", "AI", detail);
}

void livekit_app_notify_remote_audio_frame(uint32_t frame_count, uint32_t pts, uint32_t size)
{
    char agent_label[48];
    char detail[64];

    if (have_chat_history() || (s_runtime.last_text_us != 0 && (esp_timer_get_time() - s_runtime.last_text_us) < 4000000)) {
        return;
    }

    format_agent_label(agent_label, sizeof(agent_label));
    snprintf(detail, sizeof(detail), "FRAME %" PRIu32 "  PTS %" PRIu32 "  %u BYTES",
        frame_count, pts, (unsigned)size);
    lichuang_ui_show_online("REMOTE AUDIO", "AI", detail);
}

bool livekit_app_join_room(void)
{
    if (s_room_handle != NULL || s_runtime.token_connect_task_running) {
        ESP_LOGW(TAG, "Room already created");
        return true;
    }

    if (!have_credentials()) {
        app_diagnostics_note_failure("missing_credentials", "menuconfig");
        lichuang_ui_show_message("LIVEKIT", "MISSING CREDENTIALS", "CHECK MENUCONFIG", ":(");
        return false;
    }

    initialize_runtime_state();
    runtime_set_stop_requested(false);

    livekit_err_t connect_res = LIVEKIT_ERR_NONE;
#if CONFIG_LK_EXAMPLE_USE_SANDBOX
    if (!create_room_handle()) {
        return false;
    }
    livekit_sandbox_res_t res = {};
    livekit_sandbox_options_t sandbox_opts = {
        .sandbox_id = CONFIG_LK_EXAMPLE_SANDBOX_ID,
        .room_name = (char *)active_room_name(),
        .participant_identity = strlen(CONFIG_LK_EXAMPLE_PARTICIPANT_IDENTITY) > 0
            ? CONFIG_LK_EXAMPLE_PARTICIPANT_IDENTITY
            : CONFIG_LK_EXAMPLE_PARTICIPANT_NAME,
        .participant_name = CONFIG_LK_EXAMPLE_PARTICIPANT_NAME,
        .participant_metadata = strlen(CONFIG_LK_EXAMPLE_PARTICIPANT_METADATA) > 0
            ? CONFIG_LK_EXAMPLE_PARTICIPANT_METADATA
            : NULL,
        .agent_name = strlen(CONFIG_LK_EXAMPLE_AGENT_NAME) > 0
            ? CONFIG_LK_EXAMPLE_AGENT_NAME
            : NULL,
        .agent_metadata = strlen(CONFIG_LK_EXAMPLE_AGENT_METADATA) > 0
            ? CONFIG_LK_EXAMPLE_AGENT_METADATA
            : NULL,
    };

    ESP_LOGI(TAG, "Fetching sandbox token\nsandbox_id=%s\nroom=%s\nidentity=%s\nagent=%s",
        sandbox_opts.sandbox_id,
        sandbox_opts.room_name,
        sandbox_opts.participant_identity,
        sandbox_opts.agent_name != NULL ? sandbox_opts.agent_name : "(none)");

    log_livekit_network_diagnostics(NULL);

    if (!livekit_sandbox_generate(&sandbox_opts, &res)) {
        app_diagnostics_note_failure("token_failed", "sandbox");
        ESP_LOGE(TAG, "Failed to generate sandbox token");
        lichuang_ui_show_message("LIVEKIT", "TOKEN FAILED", "SANDBOX ERROR", ":(");
        destroy_room();
        return false;
    }

    log_livekit_network_diagnostics(res.server_url);
    app_diagnostics_note_stage("room_connect_call");
    connect_res = livekit_room_connect(s_room_handle, res.server_url, res.token);
    livekit_sandbox_res_free(&res);
#elif CONFIG_LK_EXAMPLE_USE_DEVICE_JWT
    if (!create_room_handle()) {
        return false;
    }
    fetched_room_credentials_t credentials = {};

    log_livekit_network_diagnostics(CONFIG_LK_EXAMPLE_SERVER_URL);
    if (!build_device_jwt_credentials(&credentials)) {
        app_diagnostics_note_failure("jwt_build_failed", "device_jwt");
        ESP_LOGE(TAG, "Failed to build device JWT");
        lichuang_ui_show_message("LIVEKIT", "JWT BUILD FAILED", "CHECK CLOCK OR SECRET", ":(");
        destroy_room();
        return false;
    }

    app_diagnostics_note_stage("room_connect_call");
    connect_res = livekit_room_connect(
        s_room_handle,
        credentials.server_url,
        credentials.token);
#elif CONFIG_LK_EXAMPLE_USE_TOKEN_SERVER
    if (!schedule_token_server_connect_task(0)) {
        app_diagnostics_note_failure("token_task_failed", "schedule_token_connect_task");
        ESP_LOGE(TAG, "Failed to start token-server connect supervisor");
        lichuang_ui_show_message("LIVEKIT", "TOKEN TASK FAILED", "CHECK HEAP", ":(");
        return false;
    }
    app_diagnostics_note_stage("token_fetch");
    lichuang_ui_show_message("LIVEKIT", "FETCHING TOKEN", active_room_name(), ":|");
    return true;
#elif CONFIG_LK_EXAMPLE_USE_PREGENERATED
    if (!create_room_handle()) {
        return false;
    }
    log_livekit_network_diagnostics(CONFIG_LK_EXAMPLE_SERVER_URL);
    app_diagnostics_note_stage("room_connect_call");
    connect_res = livekit_room_connect(
        s_room_handle,
        CONFIG_LK_EXAMPLE_SERVER_URL,
        CONFIG_LK_EXAMPLE_TOKEN);
#endif

    if (connect_res != LIVEKIT_ERR_NONE) {
        app_diagnostics_note_failure("connect_call_failed", "check_token_or_url");
        ESP_LOGE(TAG, "Failed to connect to room");
        lichuang_ui_show_message("LIVEKIT", "CONNECT CALL FAILED", "CHECK TOKEN OR URL", ":(");
        destroy_room();
        return false;
    }

    return true;
}

void livekit_app_leave_room(void)
{
    runtime_set_stop_requested(true);
    if (s_room_handle == NULL) {
        return;
    }

    livekit_room_close(s_room_handle);
    destroy_room();
}
