# HANDOFF — hk-bus-eta-rlcd Session Log

> Scan-able status log. Updated per session. Not a changelog.

---

## 1. Last Completed Step

- **[2026-07-26] "Connecting..." footer when Wi-Fi is disconnected** — Added spinlock-protected `s_wifi_connected` state shared between `wifi_event_handler` and `display_task`. On `WIFI_EVENT_STA_DISCONNECTED`: set to `false`. On `IP_EVENT_STA_GOT_IP`: set to `true`. `display_task` reads the state each render cycle: if `true`, shows "Updated HH:MM:SS"; if `false`, shows "Connecting...". Uses `portMUX_TYPE` spinlock (matching battery/weather pattern) for cross-core visibility on dual-core ESP32-S3. No new files. Build: PASS, binary 0x4c8a70 bytes (40% free).

- **[2026-07-22] Display refresh interval centralised in routes.json** — The hardcoded `10` in `display_task()` boundary calculation was replaced with `s_refresh_interval`, read once from `route_config_get_refresh_interval()` after `route_config_load()` in `app_main()`. Added validation in `route_config.c`: any `refresh_seconds` value that doesn't divide 60 evenly is rejected with a warning and clamped to the safe default of 10 s. This guarantees clean wall-clock boundary alignment for any valid interval. No other features changed. Build: PASS, binary 0x321650 bytes (~3.21 MB, 22% free). No new warnings.

- **[2026-07-20] Diagnostic ESP_LOGD added to parse_kmb_response()** — Added per-entry debug logging in the KMB parse loop to diagnose why all entries are being skipped (showing "--" for KMB routes). Each entry now logs: `eta` raw value, `api_dest`, `cfg_dest`, filter result (MATCH/MISMATCH), parse result, and final disposition (accepted/skipped with reason). Three skip reasons: missing eta, direction filter, parse_eta_epoch failure. Build: PASS, binary 0x3215f0 bytes (~3.21 MB, 22% free). No new warnings. User to flash and run with DEBUG log level to diagnose.

- **[2026-07-20] Time-based display voltage/contrast mode REMOVED** — PRD Decision #7 / FR #13 removed. Deleted `u8g2_st7305_set_voltage_profile()`, `st7305_voltage_profile_high[]`, `st7305_voltage_profile_low[]` from `u8g2_st7305.c` and `.h`. Removed the voltage-profile check block (`last_voltage_profile_high`, hour-check, `extern g_lcd` call) from `display_task` in `main.c`. Updated `PRD.md` (FR #13 deleted, §6 arch and task ownership updated, §10 Decision #7 changed to "Removed"), `CLAUDE.md` (removed "Runtime Voltage Profile Switching" section, updated runtime note and Never List), and `HANDOFF.md` (this entry). Build: PASS, binary 0x3215e0 bytes (~3.21 MB, 22% free). No new warnings. Init sequence baseline values unchanged.

- **[2026-07-17] ETA recovery fix — Wi-Fi reconnect on disconnect** — Fixed bug where display stays stuck at "--" after network restore. Root cause: the `WIFI_EVENT_STA_DISCONNECTED` handler in `wifi_event_handler()` only logged a warning and did not call `esp_wifi_connect()` — the comment incorrectly claimed the boot-time retry loop handles runtime reconnection, but that loop ends when `app_main` exits. ESP-IDF has no built-in auto-reconnect; the application must call `esp_wifi_connect()` on every disconnect event. Fix: added `esp_wifi_connect()` with 1 s backoff to prevent log flooding, and a file-scope `s_reconnect_count` reset on `IP_EVENT_STA_GOT_IP`. Also improved HTTP error logging in `http_get_body()` (added HTTP status code to the error log). Added `ESP_LOGD` fetch-cycle start log in `eta_fetch_task()` for future diagnosis. Build: PASS, binary 0x321790 bytes (~3.20 MB, 22% free). No new warnings. **Hardware-verified**: after Wi-Fi AP reboot, device reconnects within ~1 s and ETAs restore within ~30 s. No "--" stuck state observed.

- **[2026-07-14] Doc cleanup: reduced CLAUDE.md/PRD.md overlap** — PRD.md now describes requirements (what) and points to CLAUDE.md for current values (how). Refresh cadence, ETA fonts, zh-HK font specs, direction filtering details, binary size, and countdown reset values now live in CLAUDE.md only. PRD.md retains the *behaviour* (filter wrong direction at terminals, render every 15s) but defers *values* to CLAUDE.md. No code changes. Build: PASS, binary 0x31e2f0 bytes (~3.18 MB, 22% free). No new warnings.

- **[2026-07-14] KMB terminal stop direction filtering — filter by dest_en (strcasecmp)** — 238X at 海濱花園總站 (a terminal) was showing both outbound (海濱花園→中港碼頭) and inbound (中港碼頭→海濱花園) ETAs because the KMB API returns both directions for terminal stops. Fixed by adding `dest_en[64]` field to `route_config_t` in `route_config.h`, parsing it in `route_config.c`, and filtering in `eta_fetcher.c` `parse_kmb_response()` using `strcasecmp()` against each API entry's `dest_en`. Skips filtering when `dest_en` is empty (graceful fallback). Logs a warning when all entries are filtered out. Updated CLAUDE.md, PRD.md, HANDOFF.md. Build: PASS, binary 0x31e2f0 bytes (~3.18 MB, 22% free). No new warnings.

- **[2026-07-13] Fetch-failure ETA preservation — last-known-good ETAs kept on failure, expired after 3 minutes** — Fixed a bug where a failed fetch (timeout, HTTP error, JSON parse failure) would overwrite the inactive buffer with `(time_t)-1` sentinels, causing the display to show "--" for all routes until the next successful fetch. Fix: in `eta_fetch_task()` in `main.c`, when `fetch_eta()` returns `<= 0`, the code now copies the last-known-good ETA values from the active buffer into the inactive buffer (preserving stale data across the flip). A 3-minute expiry is applied: if the ETA epoch timestamp itself is more than 3 minutes (180 s) in the past per `difftime(now, eta_epoch)`, it expires to `(time_t)-1` → "--". This comparison against the ETA timestamp (not the fetch time) naturally handles both "future ETA still valid" (negative difftime → preserved) and "past ETA expired" (difftime > 180 → cleared). Build: PASS, binary 0x31c250 bytes (~3.18 MB, 22% free). No new warnings. Hardware flash/verification pending — user will flash and test.

- **[2026-07-13] KMB API endpoint switched to route-specific /eta/ + dynamic buffer growth** — Route 30X ETA for stop 92A8281D80524F78 (荃灣大河道) was failing because the KMB `/stop-eta/` endpoint returns all routes at a stop (~13 KB), overflowing the fixed 4 KB body buffer. Fix: switched KMB URL from `/stop-eta/{stop_id}` to `/eta/{stop_id}/{route}/1` (route-specific, ~700 bytes), removed the now-unnecessary route-number filter in `parse_kmb_response()`. Added dynamic buffer growth via `realloc()` in `http_event_handler()` up to a 64 KB cap, with a `buf_handle` pointer to update the caller's `buf` after realloc. Added `capture.error` check after `perform()` to reject truncated bodies instead of passing them to the JSON parser. CTB was already route-specific — no change needed. Build: PASS, binary 0x31c130 bytes (~3.18 MB, 22% free).

- **[2026-07-13] zh-HK CJK rendering verified on device + route_config.c fix** — Flashed firmware to device. Serial log showed `dest_zh`/`stop_zh` still containing English text instead of the zh-HK strings from `routes.json`. Root cause: `route_config.c` was reading `dest_en`/`stop_en` JSON keys and storing them into `dest_zh`/`stop_zh` struct fields — the `dest_zh`/`stop_zh` JSON keys were never read. Fixed: `route_config.c` now reads `dest_zh`/`stop_zh` first (zh-HK), falling back to `dest_en`/`stop_en` only if the zh-HK field is absent or empty. Updated `route_config.h` struct comments. Build clean. After reflash, serial log confirmed zh-HK text flowing through: `dest_zh='黃埔花園'`, `stop_zh='楊屋道街市'`, etc. CJK glyphs rendering correctly on the physical ST7305 display.

- **[2026-07-12] zh-HK CJK font support IMPLEMENTED** — Built custom zh-HK CJK fonts via `bdfconv` (compiled from U8g2 source `tools/bdfconv`). Two fonts generated from WenQuanYi Bitmap Song BDF files: `u8g2_font_zhhk_dest_18` (from `wenquanyi_12pt.bdf`, 12pt/16px, 27,618 glyphs, ~1.2MB) and `u8g2_font_zhhk_stop_13` (from `wenquanyi_9pt.bdf`, 9pt/12px, 27,618 glyphs, ~766KB). Map file `cjk_unified.map` covers ASCII (32–128) + CJK Unified Ideographs (U+4E00–U+9FFF) + CJK Ext A (U+3400–U+4DBF). Font files: `main/fonts/u8g2_font_zhhk_dest_18.c`, `main/fonts/u8g2_font_zhhk_stop_13.c`; header: `main/fonts/fonts.h`. `U8G2_USE_LARGE_FONTS` compile definition added to `main/CMakeLists.txt`. `main/display.c` updated to use `u8g2_DrawUTF8`/`u8g2_GetUTF8Width` for `dest_zh`/`stop_zh` fields — CJK font first, then ASCII fallback fonts for pure-ASCII strings. Partition table updated: factory app enlarged from 3 MB to 4 MB; storage partition at 0x410000. Total binary ~3.23 MB. The English-interim Helvetica fonts (`u8g2_font_helvB14_tr`/`u8g2_font_helvR10_tr`) retained as ASCII fallback path.

- **[2026-07-12] CJK font asset generation: research and plan** — Investigated `../u8g2_wqy/` repo for building custom zh-HK font subsets. Confirmed all 5 BDF files (`wenquanyi_9pt/10pt/11pt/12pt/13px.bdf`) are WenQuanYi Bitmap Song v0.9.9.8, which is a **full Unicode ISO10646-1 font** covering all 20,932 CJK Unified Ideographs (U+4E00–U+9FA5) plus Extension A — **not GB2312-only** despite the repo's `gb2312.txt` naming. The `maps/gb2312.map` etc. are character *subsets* for filtering, not encoding constraints. All 20 zh-HK test codepoints (中仁園埔屋市德楊港灣碼聯花街道銅鑼頭黃龍) confirmed present in both `wenquanyi_12pt.bdf` (16px, 41,295 glyphs) and `wenquanyi_9pt.bdf` (12px, 30,503 glyphs). Determined bdfconv should be built from source (`git clone https://github.com/olikraus/u8g2.git`, `cd tools/font/build`, `make build1`) rather than using the Windows `bdfconv.exe` via Wine. Planned two output fonts: `u8g2_font_zhhk_dest_18` (from `wenquanyi_12pt.bdf`, 16px, format 1, full CJK range) and `u8g2_font_zhhk_stop_13` (from `wenquanyi_9pt.bdf`, 12px, format 1, full CJK range). Map file `cjk_unified.map` to be generated with range `32-128,$4E00-$9FFF,$3400-$4DBF`. No code changes made yet — this was research/planning only.

- **[2026-07-12] Task decoupling: ETA fetch and display rendering into two independent FreeRTOS tasks** — Decoupled the single `while(1)` loop in `app_main()` into two real FreeRTOS tasks: `eta_fetch_task` (tskIDLE_PRIORITY+2) and `display_task` (tskIDLE_PRIORITY+3). Shared ETA data uses double-buffering (`s_route_buf[2][3]`) with an atomically-flipped active-buffer index. `eta_fetch_task` owns the ETA fetch loop, Wi-Fi modem-sleep toggling (`WIFI_PS_NONE`/`WIFI_PS_MIN_MODEM`), and a 30 s `vTaskDelay` cadence. `display_task` owns wall-clock :00/:30 alignment, voltage-profile switching, the daily NTP resync check at 06:00, and `render_dashboard()`. Added `ntp_resync()` helper with `esp_netif_sntp_deinit()`/`init()` to force a one-shot SNTP query; the boot-time `time_sync_init()` was refactored to share `ntp_wait_for_sync()`. `app_main()` now inits sequentially, creates both tasks, then calls `vTaskDelete(NULL)`. Build 0x1328d0 bytes (60% free). No mutex/semaphore used — double-buffer atomic flip is safe via aligned word-sized access on ESP32-S3.
- **[2026-07-07] Dirty-zone partial-write optimization removed** — The dirty-zone partial-write feature (`tile_overlaps_dirty_zone()`, `s_dirty_zone_mask`, `u8g2_st7305_set_dirty_zones()`) was fully removed due to a persistent header/footer tile boundary bug. `render_dashboard()` now always sends the full buffer every cycle. Active power-saving retained: Wi-Fi modem-sleep (`WIFI_PS_MIN_MODEM`) between cycles, time-based voltage profile switching (06:00–10:00 high contrast, else low voltage). Updated CLAUDE.md (added Dirty-Zone REMOVED section), PRD.md (removed partial-write references), HANDOFF.md (this file). Build 0x132550 bytes (60% free).
- **[2026-07-07] Voltage profile comment corrections** — Re-derived all voltage comments using correct datasheet formulas (§8.2.10–8.2.14). Fixed C0 (16.5V/-7.0V), C1 (5.80V), C2 (0.50V), C4 (-4.00V), C5 low value (0x1E). Updated CLAUDE.md register table and Never List.
- **[2026-07-07] Time-based display voltage/contrast mode** — Implemented `u8g2_st7305_set_voltage_profile()`. Two validated profiles. Hour-check in main.c with last-mode tracking.
- **[2026-07-07] PRD.md updated: battery-power, Tier 1 features, Tier 2/3 as TBC, FR 12 added**.
- **[2026-07-07] English interim: implemented EN text in firmware** — Helvetica fonts, shrink-to-fit, dest_en/stop_en parsing.
- **[2026-07-07] CJK root cause: font glyph coverage** — Stock wqy fonts lack most zh-HK codepoints.
- **Fixed `U8X8_WITH_USER_PTR` ABI mismatch** — PUBLIC compile definition.
- **Fixed color inversion** — Changed `0x21`→`0x20`.
- **Fixed CJK text truncation** — Changed `ST7305_TILE_WIDTH` 38→50.
- **Fixed CJK multi-character rendering (row_base pointer)** — Changed `tile->tile_ptr - x_pos*8` → `tile->tile_ptr`.

---

## 2. Files Touched This Session

| File | Change |
|------|--------|
| `main/main.c` | **Modify** — Added `s_wifi_lock` (`portMUX_TYPE`) and `s_wifi_connected` (`bool`) globals. Updated `wifi_event_handler()` to set `s_wifi_connected` on DISCONNECTED/GOT_IP events. Updated `display_task()` to show "Connecting..." instead of "Updated HH:MM:SS" when WiFi is disconnected. |
| `CLAUDE.md` | **Modify** — Added "WiFi Connection State" section documenting the spinlock pattern, state transitions, and stale-ETA behaviour. |
| `PRD.md` | **Modify** — Replaced "Reconnecting..." banner spec with "Connecting..." in footer band. Updated FR 7 footer description and Wi-Fi reconnect NFR. |
| `design.md` | **Modify** — Updated footer band description and self-verification checklist to reflect "Connecting..." text. |
| `HANDOFF.md` | **Modify** — This file. Updated §1, §2, §3, §5. |

Note: This session's previous changes (button.h/c, route_config.h/c, display.h/c, routes.json, CMakeLists.txt, design.md, CLAUDE.md, PRD.md) are recorded in the prior session's §2 and §1. The §2 table above only reflects the current session's changes.

---

## 3. Build Status

- **Last build**: PASS — `idf.py build` completed. Binary 0x4c8a70 bytes (~4.78 MB, 8 MB factory partition, 40% free). Added "Connecting..." footer text when Wi-Fi disconnected. No new warnings.

---

## 4. Known Issues / Open Questions

- **CJK zh-HK rendering**: WORKING END-TO-END — Verified on physical display. `routes.json` provides `dest_zh`/`stop_zh` fields. `route_config.c` reads zh-HK fields first (falls back to `_en` if absent). `display.c` uses `u8g2_DrawUTF8`/`u8g2_GetUTF8Width` with custom fonts (`u8g2_font_zhhk_dest_24`, `u8g2_font_zhhk_stop_20`).
- **KMB body capture overflow**: FIXED — KMB now uses route-specific `/eta/` endpoint (~700 bytes). Dynamic buffer growth (realloc) added as safety net.
- **Fetch failure ETA preservation**: FIXED — Failed fetches now preserve last-known-good ETAs for up to 3 minutes, then expire to "--".
- **Wi-Fi disconnect stuck at "--"**: FIXED — Added `esp_wifi_connect()` to `WIFI_EVENT_STA_DISCONNECTED` handler. The boot-time retry loop was the only reconnect mechanism, but it exits when `app_main` returns. ESP-IDF has no built-in auto-reconnect.
- **"Connecting..." footer**: IMPLEMENTED — Footer shows "Connecting..." when WiFi is disconnected (boot or runtime), reverts to "Updated HH:MM:SS" on reconnection. Uses spinlock-protected bool for cross-core safety.
- **Tier 2 (adaptive/night-mode refresh) and Tier 3 (low-battery UX) deferred** — Documented in PRD.md §9 Open/TBC Decisions. Not implemented.
- **GPIO18 conflict with pending sleep plan** — Page-toggle now owns GPIO18 short-press. The pending sleep plan (`docs/plan-display-sleep-button-wake.md`) must be reworked (long-press discriminator or different button) when implemented.

---

## 5. Next Step

1. **Test "Connecting..." footer on physical device** — Flash the new firmware and verify the footer shows "Connecting..." when Wi-Fi is disconnected and switches to "Updated HH:MM:SS" on reconnection. Validate spinlock cross-core correctness.
2. **Continue feature work** — After verification, consider: Display sleep + button wake (plan exists at `docs/plan-display-sleep-button-wake.md`). **Note**: GPIO18 now claimed by page-toggle — sleep plan must be reworked (long-press discriminator or different button).