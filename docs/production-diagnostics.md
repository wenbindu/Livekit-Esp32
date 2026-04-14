# Production Diagnostics

## Current Baseline

The firmware now includes a minimal persistent diagnostics baseline for
`ESP32-S3` startup and connection troubleshooting.

Implemented behavior:

- capture `esp_reset_reason()` on every boot
- persist `boot_count`
- persist `session_id`
- persist `reboot_streak` for consecutive unstable boots
- persist the latest lifecycle breadcrumb
- persist a short detail string for the latest failure or reset context
- emit a startup summary log line after NVS initialization

Implementation files:

- `main/app_diagnostics.c`
- `main/app_diagnostics.h`
- `main/app_main.c`
- `main/livekit_app.c`

## Stored State

Diagnostics state is stored in NVS namespace:

- `app_diag`

Keys currently tracked:

- `boot_count`
- `reboot_stk`
- `boot_ok`
- `session_id`
- `breadcrumb`
- `detail`

## Breadcrumbs

The firmware now records key lifecycle stages such as:

- `boot_start`
- `system_init`
- `wifi_connect`
- `wifi_connected`
- `standby_wait`
- `chat_button_pressed`
- `time_sync`
- `audio_init`
- `room_join_start`
- `token_fetch`
- `room_connect_call`
- `room_connecting`
- `room_connected`

Failure breadcrumbs now include cases such as:

- `wifi_failed`
- `token_fetch_failed`
- `token_task_failed`
- `room_create_failed`
- `room_connect_failed`
- `room_disconnected`
- `room_failed`
- `auth_expired`

## Stable Boot Rule

The current implementation marks a boot as stable when the device reaches one
of these states:

- `standby_wait`
- `local_audio_only`
- `room_connected`

If the previous boot did not reach a stable state, the next boot increments
`reboot_streak`.

## Startup Summary

At startup, the firmware logs a summary line similar to:

```text
diag_boot session=... boot_count=... reboot_streak=... reset_reason=... previous_boot_completed=... previous_breadcrumb=... previous_detail=...
```

This is the first useful line to check when diagnosing:

- repeated resets
- reconnect loops after reboot
- crashes before room join

## What This Does Not Yet Cover

This baseline does not yet include:

- coredump to flash
- deferred diagnostics upload to the server
- log ring buffer export
- server-side diagnostics ingest APIs

Those are the next production-hardening steps.
