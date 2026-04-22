# Memory Audit TODO

## Goal

Explain why `PSRAM` still has multiple megabytes free while `internal heap` is
close to exhaustion, then trim the firmware without breaking the normal chat
flow on the Lichuang ESP32-S3 board.

## Current Snapshot

- Board resources:
  - `16MB` flash
  - `8MB` PSRAM
- Current partition layout:
  - `factory` app: `8MB`
  - `storage` SPIFFS: `0x7D0000` bytes
- Static internal usage from `idf_size.py`:
  - `DIRAM used = 212,395 bytes`
  - `DIRAM remain = 129,365 bytes`
- Runtime heap observed from serial logs:
  - token fetch start: `internal_free=60,659`, `largest=49,152`, `spiram_free=5,837,032`
  - room connected: `internal_free=48,687`, `largest=40,960`, `spiram_free=4,781,508`

## Status Update

### 2026-04-15: LVGL widget trim pass

Applied the first low-risk trim pass for LVGL:

- disabled unused widget families in `configs/sdkconfig.defaults.base`
- disabled `LV_USE_LODEPNG`
- disabled `LV_USE_IMGFONT`
- disabled `LV_USE_OBSERVER`
- updated `components/78__xiaozhi-fonts/CMakeLists.txt` so `font_emoji_32.c`
  and `font_emoji_64.c` are excluded when `IMGFONT` is disabled

Verification result:

- `bash scripts/project.sh configure` passed
- `bash scripts/project.sh build` passed
- current `idf_size.py` summary:
  - `DIRAM used = 211,667 bytes`
  - `DIRAM remain = 130,093 bytes`
  - `app bin size = 0x349af0`

What this means:

- compared with the earlier snapshot (`212,395` DIRAM used), this pass only
  recovered about `728 bytes` of static DIRAM
- that is expected because the main LVGL internal-memory cost is still the
  built-in LVGL heap pool:
  - `CONFIG_LV_MEM_SIZE_KILOBYTES=32`
- current archive summary still shows:
  - `liblvgl__lvgl.a` total about `221,978 bytes`
  - `lib78__xiaozhi-fonts.a` total about `263,906 bytes`, but almost entirely
    in flash / rodata, not DIRAM

Conclusion:

- item 1 is worth keeping because it removes unused LVGL surface area and makes
  the config cleaner
- but it does not materially solve internal-heap pressure by itself
- the next high-value LVGL step is to test:
  - `CONFIG_LV_MEM_SIZE_KILOBYTES=16`
  - then possibly `8`

### 2026-04-15: LVGL heap size validation

Validated the next LVGL heap-size step on real hardware.

Static size results:

- `32KB` LVGL heap after widget trim:
  - `DIRAM used = 211,667 bytes`
  - `DIRAM remain = 130,093 bytes`
- `16KB` LVGL heap:
  - `DIRAM used = 195,283 bytes`
  - `DIRAM remain = 146,477 bytes`
- net recovery versus the `32KB` build:
  - `16,384 bytes` of DIRAM

Runtime validation:

- `16KB` was flashed to the Lichuang ESP32-S3 and booted normally
- UI initialization passed:
  - `lichuang_ui: LVGL buffer lines=12 trans_lines=0 psram=0 task_stack=internal`
  - `LVGL: Starting LVGL task`
  - `lichuang_ui: Lichuang LVGL UI initialized`
- the device continued through:
  - Wi-Fi association
  - token fetch from `device_server`
  - LiveKit room connection
  - publisher DTLS/SRTP setup
- room connection reached:
  - `livekit_room: Room state changed: Connected`
  - `Heap[room-connected]: internal_free=68319 internal_largest=61440 internal_min=63315 spiram_free=4944556`

`8KB` result:

- `8KB` compiled and flashed successfully
- on hardware it was not stable
- around `6.6s` after boot, the task watchdog fired with CPU1 stuck in
  `taskLVGL`
- backtrace top frames were inside label drawing:
  - `lv_draw_unit_draw_letter`
  - `lv_draw_label_iterate_characters`
  - `lv_draw_sw_label`
  - `lv_label_event`
  - `lv_display_refr_timer`
  - `lv_timer_handler`
  - `lvgl_port_task`

Decision:

- keep `CONFIG_LV_MEM_SIZE_KILOBYTES=16`
- reject `8KB` as the default because it fails on-device
- item 1 is now complete for the current UI path
- later disconnects still appeared during LiveKit runtime, but that is a
  separate networking/RTC issue, not an LVGL heap-size failure

### 2026-04-15: Wi-Fi + LWIP low-risk trim pass

Applied the first low-risk networking trim pass:

- disabled `CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT`
- disabled `CONFIG_LWIP_IPV6`
- intentionally kept WPA3 / OWE support enabled for now to avoid breaking
  common modern APs during daily development

Code changes needed to make the IPv4-only build work:

- updated `main/livekit_app.c` so DNS and host-resolution diagnostics only
  compile IPv6-specific branches when `CONFIG_LWIP_IPV6=y`
- added `main/ipv6_compat_stubs.c`
- updated `main/CMakeLists.txt` to link the compatibility symbols explicitly

Why the compatibility stubs are needed:

- `espressif__esp_peer` ships `libpeer_default.a` as a prebuilt library
- even when runtime peer config sets `ipv6_support = false`, that prebuilt
  library still links IPv6 helper symbols:
  - `esp_netif_get_all_ip6`
  - `esp_netif_ip6_get_addr_type`
  - `in6addr_any`
- with `CONFIG_LWIP_IPV6=n`, ESP-IDF no longer provides those symbols
- the stubs make those symbols resolve to an IPv4-only behavior:
  - no IPv6 addresses available
  - IPv6 address type unknown
  - wildcard IPv6 bind address stays zeroed

Verification result:

- `bash scripts/project.sh configure` passed
- `bash scripts/project.sh build` passed
- current `idf_size.py` summary:
  - `DIRAM used = 193,651 bytes`
  - `DIRAM remain = 148,109 bytes`
  - `app bin size = 0x3428c0`

What this means:

- compared with the earlier `16KB` LVGL baseline (`195,283` DIRAM used), this
  pass recovered another `1,632 bytes` of DIRAM
- compared with the `32KB` LVGL baseline (`211,667` DIRAM used), total recovery
  is now `18,016 bytes`

Hardware validation:

- flashed successfully to the Lichuang ESP32-S3
- boot completed normally
- Wi-Fi driver initialized normally
- station scan loop worked
- in the test environment, no saved AP was found, so the device timed out and
  correctly fell back to the provisioning portal
- fallback provisioning AP came up successfully:
  - `WifiConfigurationAp: Access Point started with SSID LK-Lichuang-10A1`
  - `wifi_connect: Open provisioning portal: AP=LK-Lichuang-10A1 URL=http://192.168.4.1`
- after the network was restored, full end-to-end validation also passed:
  - saved SSID association succeeded
  - token fetch from `device_server` succeeded
  - LiveKit room connection succeeded
  - downlink subscription reached the renderer path:
    - `livekit_engine: Subscribing to audio track`
    - `livekit_peer.sub: RTC state changed to CONNECTED (7)`
    - `AV_RENDER: Get need resample 1 in:16000 out:16000`
    - `I2S_RENDER: open channel:1 sample_rate:16000 bits:16`
    - `livekit_room: Room state changed: Connected`
    - `livekit_peer.pub: RTC state changed to DATA_CHANNEL_OPENED`
  - runtime heap snapshot on the recovered network:
    - token fetch start:
      - `internal_free=81295`
      - `internal_largest=63488`
      - `internal_min=64759`
      - `spiram_free=6003908`
    - room connected:
      - `internal_free=68655`
      - `internal_largest=63488`
      - `internal_min=64759`
      - `spiram_free=4846332`

Decision:

- keep `CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT=n`
- keep `CONFIG_LWIP_IPV6=n`
- keep WPA3 / OWE support enabled for now
- item 2 is complete:
  - compile-time validation passed
  - boot / scan / provisioning fallback validation passed
  - saved-SSID -> `device_server` -> LiveKit room validation passed
  - downlink reached the playback path successfully
  - raw downlink capture/export is still disabled and must be enabled
    separately if we want to inspect PCM artifacts

### HTTP Downlink Capture Smoke Test

Goal:

- validate the `downlink-http` diagnostic preset on real hardware
- confirm rendered downlink WAV capture can be enabled without bringing the
  device down before room join

Preset used:

- `FIRMWARE_PRESET=downlink-http`
- `DEBUG_DOWNLINK_HTTP_RINGBUF_KB=24`
- `DEBUG_DOWNLINK_HTTP_IDLE_FLUSH_MS=1000`
- `DEBUG_DOWNLINK_HTTP_SEGMENT_SECONDS=8`

Observed runtime after flashing the preset:

- capture init succeeded:
  - `Downlink capture storage ready: total=7522721 used=3481370 free=4041351`
  - `Downlink HTTP capture enabled: ringbuf=24KB idle_flush_ms=1000 max_segment_seconds=8`
- token task start:
  - `internal_free=64099`
  - `internal_largest=63488`
  - `internal_min=64015`
  - `spiram_free=5975948`
- room connected:
  - `internal_free=52863`
  - `internal_largest=51200`
  - `internal_min=52863`
  - `spiram_free=4930408`

What this means:

- the preset is viable on the board: Wi-Fi, token fetch, signaling, ICE, DTLS,
  and room join still complete
- compared with the earlier non-capture snapshot (`internal_free=68655` at room
  connected), enabling HTTP downlink capture costs roughly `15.8KB` of internal
  heap at steady state
- this is expected because the preset adds:
  - one writer task with internal stack
  - one uploader task with internal stack
  - an internal upload queue
  - SPIFFS and HTTP client runtime state

Residual risk:

- this preset is for short-lived diagnostics only; it should remain default-off
  on normal `dev`, `test`, and `main`
- during monitor the device later hit `Failure reason: Ping Timeout`, so the
  older intermittent disconnect problem is still not fully closed

## Why This Happens

`PSRAM` and `internal RAM` are different pools.

- Many objects can use PSRAM:
  - large task stacks
  - audio ring buffers
  - non-DMA runtime allocations
- Many objects still require internal RAM:
  - IRAM / DIRAM code and static data
  - DMA-capable display buffers
  - flash/cache-safe code paths
  - some task stacks
  - small allocations biased toward internal RAM

Current config also reserves internal RAM on purpose:

- `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=256`
- `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=98304`

That is correct for system stability, but it makes internal-heap pressure show
up much earlier than PSRAM pressure.

## Biggest Internal Consumers

### 1. LVGL

What it does now:

- drives the LCD UI
- shows boot, Wi-Fi provisioning, standby, and online/chat state
- is used by:
  - `main/lichuang_ui.cc`
  - `main/wifi_connect.cc`
  - `main/livekit_app.c`

Known internal-memory costs:

- built-in LVGL heap pool: `16KB` in the current validated build
  - `CONFIG_LV_MEM_SIZE_KILOBYTES=16`
  - previous `32KB` baseline consumed another `16,384 bytes` of DIRAM
- LVGL task stack: default `7168` bytes, internal stack by default
- display draw buffer:
  - current size is `320 * 12 lines * 2 bytes = 7,680 bytes`
  - currently allocated as DMA-capable internal memory

What appears unused in the current UI implementation:

- the current UI only uses a small subset:
  - base objects
  - labels
  - image
  - timer
  - align / style / flex helpers
- many enabled LVGL widgets are not referenced by `main/lichuang_ui.cc`, such as:
  - calendar
  - chart
  - checkbox
  - dropdown
  - keyboard
  - led
  - list
  - menu
  - msgbox
  - roller
  - scale
  - spinbox
  - spinner
  - textarea
  - table
  - tabview
  - tileview
  - win
- `LV_USE_LODEPNG=y` also looks suspicious because the current UI uses compiled
  C assets for emoji and fonts, not runtime PNG decoding

Trim candidates:

- keep LVGL pool at `16KB`
- do not reduce to `8KB` with the current UI draw path
- move display buffer to PSRAM if panel path remains stable
- try moving LVGL task stack to PSRAM
- disable unused widgets and image decoders
- define a `headless` or `minimal-ui` production profile if LCD is optional

### 2. Wi-Fi + LWIP

What it does now:

- station connection to the local router
- SoftAP provisioning portal when saved credentials are missing or BOOT is held
- DHCP, DNS, sockets, HTTP portal, TLS client traffic to:
  - `device_server`
  - LiveKit signaling
  - TURN / RTC related networking

What must remain for the current product flow:

- STA mode
- SoftAP mode if browser provisioning is still required
- IPv4
- DHCP client
- DHCP server for the provisioning AP
- DNS client
- TCP / UDP sockets
- TLS client

What looks optional or worth auditing:

- `CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT`
  - already disabled in the current trim pass
  - safe unless the product must join 802.1X / EAP enterprise networks
- WPA3 / OWE / SAE extras
  - still enabled for compatibility with modern personal Wi-Fi networks
  - worth trimming only if deployment policy is clearly WPA2-only
- `CONFIG_LWIP_IPV6`
  - already disabled in the current trim pass
  - required one compatibility shim because the prebuilt `esp_peer` library
    still references IPv6 helper symbols at link time
- Wi-Fi buffer counts may be larger than necessary for this product
  - these need real network regression testing before reduction

Trim candidates:

- keep enterprise Wi-Fi support disabled unless explicitly needed
- decide whether WPA3 / OWE / SAE are hard product requirements
- tune Wi-Fi RX/TX buffer counts after reliability testing

### 3. FreeRTOS

What it does now:

- task scheduler
- queues
- event groups
- timers
- synchronization primitives
- system background tasks required by ESP-IDF and networking

What must remain:

- FreeRTOS itself is not optional
- timer service, queues, event groups, and task support are all used directly by the app

What is likely oversized or worth auditing:

- `lk_app` task stack: `16KB`
- `diag_upload` task stack: `8KB`
- `diag_flush` task stack: `4KB`
- `time_sync` task stack: `4KB`
- LVGL task stack: `7168`
- internal-only downlink capture tasks: `6KB` and `8KB`
- system task stacks:
  - main task
  - esp timer task
  - lwip tcpip task
  - freertos timer task

Trim candidates:

- measure stack high-water marks for all app-created tasks
- reduce task stacks where there is at least `25%` safe headroom
- keep large stacks on PSRAM where flash/cache rules allow it
- re-evaluate FreeRTOS task snapshot features if not required in production

### 4. Flash / HW Support

What it does now:

- flash read / write / erase support
- PSRAM init and cache coordination
- SPI / HAL / low-level driver support
- coredump to flash
- IRAM-safe runtime pieces for system stability

What must remain:

- core flash support
- PSRAM bring-up
- SPI / HAL / cache glue
- board drivers for audio and display

What is optional or policy-driven:

- flash coredump
- log code in IRAM
- timer code in IRAM
- some production diagnostics knobs

These options are not "free":

- `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`
- `CONFIG_LOG_IN_IRAM=y`
- `CONFIG_ESP_TIMER_IN_IRAM=y`
- `CONFIG_SPI_FLASH_PLACE_FUNCTIONS_IN_IRAM=y`

Trim candidates:

- decide whether development and production should share the same coredump policy
- decide whether IRAM-heavy logging is really needed outside failure-critical paths
- keep crash forensics only where they materially improve field debugging

## TODO

- [ ] Add a build note with the latest static memory snapshot and runtime heap snapshot.
- [ ] Measure task stack high-water marks for:
  - `lk_app`
  - `taskLVGL`
  - `diag_upload`
  - `diag_flush`
  - `dbg_downlink_cap`
  - `dbg_downlink_up`
  - `time_sync`
- [ ] Reduce `CONFIG_LV_MEM_SIZE_KILOBYTES` from `32` to `16` and verify boot, Wi-Fi portal, standby, and chat UI.
- [ ] Disable unused LVGL widgets and decoders not referenced by `main/lichuang_ui.cc`.
- [ ] Test `CONFIG_LK_EXAMPLE_UI_USE_PSRAM_BUFFER=y` and verify LCD stability.
- [ ] Decide whether production firmware needs full LCD UI or only minimal status output.
- [ ] Audit whether `CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT` is a real product requirement.
- [ ] Decide whether WPA3 / OWE / SAE are required for the target deployment.
- [ ] Test an IPv4-only build and verify LiveKit signaling, TURN, and provisioning still work.
- [ ] Evaluate smaller Wi-Fi buffer counts only after link-stability testing on weak networks.
- [ ] Audit diagnostics tasks so development-only capture paths are not always-on in normal firmware.
- [ ] Decide whether flash coredump stays enabled in:
  - `dev`
  - `test`
  - `main`
- [ ] Review `LOG_IN_IRAM`, `ESP_TIMER_IN_IRAM`, and related IRAM-heavy options against actual runtime needs.

## Immediate Suspects

If the goal is to recover internal RAM quickly with the lowest code risk, start
here:

1. LVGL internal heap pool (`32KB`)
2. LVGL task stack and draw buffer
3. development diagnostics tasks that require internal stacks
4. enterprise Wi-Fi support
5. IPv6 if the product can be IPv4-only
