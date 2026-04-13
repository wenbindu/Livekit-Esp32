#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t lichuang_ui_init(void);
esp_err_t lichuang_ui_suspend(void);
esp_err_t lichuang_ui_resume(void);
void lichuang_ui_show_message(const char *title, const char *line1, const char *line2, const char *emoji);
void lichuang_ui_show_boot_prompt(int remaining_seconds);
void lichuang_ui_show_online(const char *title, const char *line1, const char *line2);
void lichuang_ui_stop_animation(void);

#ifdef __cplusplus
}
#endif
