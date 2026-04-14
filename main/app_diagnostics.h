#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_system.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t session_id;
    uint32_t boot_count;
    uint32_t reboot_streak;
    uint32_t coredump_size;
    uint32_t coredump_exc_pc;
    bool previous_boot_completed;
    bool coredump_present;
    esp_reset_reason_t reset_reason;
    char previous_breadcrumb[32];
    char previous_detail[64];
    char current_breadcrumb[32];
    char current_detail[64];
    char coredump_task[16];
    char coredump_reason[96];
} app_diagnostics_snapshot_t;

void app_diagnostics_init(void);
const app_diagnostics_snapshot_t *app_diagnostics_snapshot(void);
void app_diagnostics_log_boot_summary(void);
void app_diagnostics_note_stage(const char *breadcrumb);
void app_diagnostics_note_failure(const char *breadcrumb, const char *detail);
void app_diagnostics_mark_boot_stable(const char *breadcrumb);
const char *app_diagnostics_reset_reason_str(esp_reset_reason_t reason);

#ifdef __cplusplus
}
#endif
