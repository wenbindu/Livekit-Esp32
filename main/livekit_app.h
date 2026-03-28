#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool livekit_app_join_room(void);
void livekit_app_leave_room(void);

#ifdef __cplusplus
}
#endif
