# Project: hk-bus-eta-rlcd

## Fixed Technical Stack
- Target: ESP32-S3-RLCD 4.2" (ST7305 driver, 400x300, 1-bit
  monochrome, no backlight)
- Framework: ESP-IDF (native), NOT Arduino
- Display library: U8g2 — NOT LVGL (see design.md for rationale)
- Data sources: KMB ETA Open API + Citybus ETA Open API (two
  separate response shapes — never assume they're unified)
- Config format: routes.json via SPIFFS, NOT hardcoded values
- Time sync: SNTP, Asia/Hong_Kong (HKT, UTC+8)

## File & Path Conventions
- docs/waveshare-pinout.md, docs/ESP32-S3-RLCD-4.2-schematic.pdf,
  docs/ST_7305_V0_2.pdf — board reference docs
- PRD.md — source of truth for requirements; read first
- design.md — source of truth for layout/rendering rules
- HANDOFF.md — running session-state log; read first on resume,
  update after every build

## Waveshare Reference Repository

- **Location**: `../waveshare-reference/` (git clone of
  `waveshareteam/ESP32-S3-RLCD-4.2`)
- **Primary driver reference**: `02_Example/ESP-IDF/11_U8G2_Test/
  components/u8g2_st7305/` — This is the **official Waveshare U8g2
  driver** for this board. Use its init sequence, SPI config, pixel
  layout, and register values as the authoritative starting point.
  Any deviation from this driver must be justified.
- **Secondary references**:
  - `02_Example/ESP-IDF/10_FactoryProgram/` — Factory firmware
    using `DisplayPort` class (raw framebuffer + LUT-based pixel
    mapping)
  - `02_Example/XiaoZhi/XiaoZhiCode_V2.1.0/` — LVGL firmware with
    `RLCD_SetPixel` and pixel lookup tables (40 MHz SPI)
  - `02_Example/Arduino/10_U8G2_Test/` — Arduino variant of the
    same custom U8g2 driver

## ST7305 Register Values — Starting Point from Waveshare Reference

The init sequence in `st7305_full_init()` inside
`u8g2_st7305.c` is the **confirmed, working baseline** for this
exact board. Our `display_init()` should reproduce these values
exactly, not values from the generic datasheet §7.9.2:

| Reg | Waveshare (u8g2_st7305) | Notes |
|-----|------------------------|-------|
| 0xC0 | `{0x11, 0x04}` | VGH=16.5V, VGL=-7.0V (GCTRL, §8.2.10) |
| 0xC1 | `{0x69,0x69,0x69,0x69}` | VSHP = 5.80V (§8.2.11) |
| 0xC2 | `{0x19,0x19,0x19,0x19}` | VSLP = 0.50V (§8.2.12) |
| 0xC4 | `{0x4B,0x4B,0x4B,0x4B}` | VSHN = -4.00V (§8.2.13) |
| 0xC5 | `{0x19,0x19,0x19,0x19}` | VSLN = 0.50V (§8.2.14, negative slope) |
| 0xB2 | `{0x02}` | Frame rate HPM |
| 0xB0 | `{0x64}` (100 decimal = 400 lines) | Duty setting |
| 0x62 | `{0x32,0x03,0x1F}` | Gate timing (missing from our init) |
| 0x3A | `{0x11}` (4GS — 4-grey-scale) | Pixel format |
| 0x21 | Display Inversion ON | Command (missing from our init) |

**Runtime note**: Registers 0xC0–C5 are the init-time baseline only. They are no longer varied at runtime — the voltage-profile switching feature has been removed (see HANDOFF.md for history).

### CRITICAL: CASET/RASET and pixel layout
Waveshare uses a **non-standard pixel layout**:
- **`u8g2_ll_hvline_vertical_top_lsb`** + **`U8G2_R1`** (90° rotation)
- CASET (0x2A): 2-byte format `{0x12, 0x2A}` mapping 24 columns of
  12 pixels each (24×12 = 288 native, trimmed to 300 via rotation)
- RASET (0x2B): 2-byte format `{0x00, 0xC7}` mapping 200 rows of
  2 pixels each (200×2 = 400)
- A custom DRAW_TILE callback remaps the U8g2 horizontal tile buffer
  into the ST7305's native 12×4-pixel-group addressing using a 4×4
  lookup table

Our project currently uses a **row-major MSB-left** approach with
standard 4-byte CASET/RASET. This is incompatible with copying
Waveshare's register values directly. Any adoption of Waveshare's
init values must be paired with the correct pixel layout strategy.

### SPI configuration baseline
- **SPI3_HOST** (not SPI2_HOST)
- **24 MHz** clock (not 40 MHz)
- **CS/DC managed manually via GPIO** (not hardware CS)
- **`spi_device_polling_transmit`** (blocking, no queue)
- **PSRAM** for display buffer allocation (8 MB available)

### Reset timing
- Waveshare: HIGH(50ms) → LOW(20ms) → HIGH(50ms)
- Our code: LOW(10ms) → HIGH(120ms)

## Dirty-Zone Partial-Write Optimization (REMOVED)
The dirty-zone partial-write optimization (tile_overlaps_dirty_zone(),
s_dirty_zone_mask, u8g2_st7305_set_dirty_zones()) was implemented
and then removed due to a header/footer tile boundary bug that
could not be reliably fixed. The DRAW_TILE callback now always
writes every tile via SPI on every u8g2_SendBuffer() call. The
Wi-Fi modem-sleep between cycles remains in place as the sole
active power-saving feature. See HANDOFF.md for the session
history of this feature's lifecycle.

## Permanent Never List
- Never use LVGL (this board has no PSRAM requirement assumption
  and U8g2 is the confirmed library — see PRD.md Hardware
  Constraints)
- Never use generic ST7305 datasheet §7.9.2 init values — always
  use the Waveshare reference driver values as the starting point
  (see §ST7305 Register Values above). Any other
  deviation from Waveshare reference values still requires
  justification.
- Never hardcode Wi-Fi credentials — always via config file
- Never assume Citybus and KMB JSON shapes are interchangeable
  (both are now route-specific, but their field naming and response
  structures differ)
- Never invent pin numbers not confirmed in docs/waveshare-pinout.md
- Never store precomputed time-relative values (e.g. "minutes until
  arrival") across task/render boundaries — always store raw epoch
  timestamps and recompute at render time. Precomputed minutes go
  stale between fetch cycles and compound error when a fetch is
  delayed, retried, or jittered, causing the displayed countdown to
  lag behind real time.
- **Direction filtering**: KMB terminal stops return both outbound and
  inbound ETAs. The `dest_en` field from routes.json is used with
  `strcasecmp()` in `parse_kmb_response()` to filter out the opposite
  direction. If `dest_en` is empty in routes.json, filtering is skipped
  (all directions shown).
- **ADC1 only, never ADC2, when Wi-Fi is active**: ADC2 conflicts with
  the active Wi-Fi radio on the ESP32-S3. The battery voltage ADC uses
  ADC1 (channel 3, GPIO4) exclusively. ADC_ATTEN_DB_12, curve-fitting
  calibration via `adc_cali_create_scheme_curve_fitting`, 16-sample
  averaging, 3× on-board voltage divider. Piecewise linear LUT (11-point
  Li-ion discharge curve) for voltage-to-percentage mapping, with
  hysteresis (1% deadband, first read always accepted) to prevent
  display flicker from ADC noise. Error/uninitialized sentinel value is
  255, displayed as `Battery:  --%` — never "0%".
- **Never sample ADC during or near active Wi-Fi TX bursts**: The
  ESP32-S3 Wi-Fi TX current draw (~300-400 mA peak) causes measurable
  voltage sag on the shared battery rail. Any ADC/sensor reading that
  coincides with a Wi-Fi TX event will capture a sagged voltage, not
  the true resting voltage. This is a hardware-level caveat, not
  specific to the battery feature — any future ADC sensor reading on
  this board must either (a) sample during a confirmed Wi-Fi-idle
  window (after `WIFI_PS_MIN_MODEM` re-enable with a settle delay),
  or (b) apply median filtering to discard single TX-sag outliers.
  Current battery implementation uses both (a) and (b):
  `battery_sample_if_due()` is called from `eta_fetch_task` after
  `WIFI_PS_MIN_MODEM`, with a 50 ms settle delay, rolling 5-sample
  median filter, EMA (α=0.2), and two-reading confirmation for jumps
  >6 points. `battery_get_percentage()` returns a cached value (no ADC
  read).
- **Never omit `esp_wifi_connect()` from the `WIFI_EVENT_STA_DISCONNECTED`
  handler**: ESP-IDF has no built-in auto-reconnect. The
  `failure_retry_cnt` field in `wifi_config_t` only controls the
  initial connection during `esp_wifi_start()`, not runtime
  reconnection. After a disconnect at runtime, the application must
  call `esp_wifi_connect()` again — typically in the
  `WIFI_EVENT_STA_DISCONNECTED` event handler — or Wi-Fi will stay
  disconnected indefinitely. The standard ESP-IDF pattern is:
  `esp_wifi_connect()` on both `WIFI_EVENT_STA_START` and
  `WIFI_EVENT_STA_DISCONNECTED`, with a small backoff delay (e.g. 1 s)
  to avoid log flooding on repeated disconnects.

## Data Source Handling
- KMB uses the route-specific `/eta/{stop_id}/{route}/{service_type}` endpoint
  which returns only the requested route. Citybus uses `/eta/CTB/{stop_id}/{route}`
  — both are route-specific, but the response shapes still differ in field naming
  and structure, so parsing logic must not assume interchangeability.
- Null ETA fields render as "--", never "0" (see PRD.md)

## zh-HK CJK Font Support (WORKING — end-to-end verified)
- Destination and bus-stop name fields render in **zh-HK Chinese** on
  the physical display, verified end-to-end from `routes.json` →
  `route_config.c` → `display.c` → ST7305.
- **Custom fonts** (generated via `otf2bdf` + `bdfconv` from Noto Sans CJK HK):
  - `u8g2_font_zhhk_dest_24` — 24px Noto Sans CJK HK Bold, 27,942 glyphs, ~2.0 MB
  - `u8g2_font_zhhk_stop_20` — 20px Noto Sans CJK HK Regular, 27,942 glyphs, ~1.6 MB
- **Coverage**: ASCII (32-128) + CJK Symbols and Punctuation (U+3000–U+303F)
  + CJK Unified Ideographs (U+4E00–U+9FFF) + CJK Extension A (U+3400–U+4DBF)
  + Halfwidth and Fullwidth Forms (U+FF00–U+FFEF)
- **Data flow**: `routes.json` provides `dest_zh`/`stop_zh` fields (zh-HK
  text). `route_config.c` reads `dest_zh`/`stop_zh` first, falling back
  to `dest_en`/`stop_en` only if the zh-HK field is absent or empty.
  `display.c` uses `u8g2_DrawUTF8`/`u8g2_GetUTF8Width` for `dest_zh`
  and `stop_zh` fields (CJK font first, ASCII fallback fonts for
  pure-ASCII strings).
- **Partition table**: factory app enlarged from 4 MB to 8 MB to
  accommodate larger font data; storage partition moved to 0x810000.
- **Total binary**: ~4.8 MB (0x4c8c90 bytes).
- Font files: `main/fonts/u8g2_font_zhhk_dest_24.c`,
  `main/fonts/u8g2_font_zhhk_stop_20.c`; header: `main/fonts/fonts.h`.
- `U8G2_USE_LARGE_FONTS` compile definition added to
  `main/CMakeLists.txt` to support the large font data arrays.
- The English-interim Helvetica fonts (`u8g2_font_helvB14_tr`/
  `u8g2_font_helvR10_tr`) remain as a fallback path for pure-ASCII
  strings (e.g. if `dest_zh` is absent in `routes.json`).
- **"往" prefix**: The destination line is prefixed with "往" (drawn
  in the stop-font size, `u8g2_font_zhhk_stop_20`) to indicate
  direction, followed by the destination name in `u8g2_font_zhhk_dest_24`.
- **ETA fonts**: eta1 (soonest) uses `u8g2_font_profont29_mf` (29px bold),
  eta2/eta3 use `u8g2_font_profont17_mf` (17px regular). The "min" suffix
  remains `u8g2_font_profont12_mf`.

## Code Style
- esp_err_t checked on every ESP-IDF call, no silent failures
- No unbounded buffers; validate JSON field presence before use
- Comment any placeholder/stub function explicitly as such

## Session Workflow
- Read HANDOFF.md first on session resume to identify last completed
  step and next action
- After any code-generation task (writing/modifying .c/.h,
  CMakeLists.txt, or sdkconfig):

  Update HANDOFF.md §3 (Build Status) and §1 (Last Completed Step) to reflect the verified outcome
  
  - Do not regenerate the entire file — only update relevant sections

## ETA Fonts (current)
- eta1 (soonest): `u8g2_font_profont29_mf` (29px, bold)
- eta2, eta3: `u8g2_font_profont22_mf` (22px, regular)
- "min" suffix: `u8g2_font_profont12_mf` (unchanged)

## Route Number Font (current)
- `u8g2_font_profont29_mf` (29px, bold) — same as eta1

## Display Refresh
- Display renders at configurable wall-clock boundaries (read from `routes.json` `refresh_seconds`). Valid values are divisors of 60 (e.g. 10, 12, 15, 20, 30) — any value that does not divide 60 evenly is rejected at boot with a warning and clamped to 10 s.
- ETA fetch runs independently at ~30s interval with ±10% random jitter (27–33s, using `esp_random()`) to avoid thundering-herd alignment
- Refresh interval configured via `routes.json` `refresh_seconds`

## Header Date
- The left-aligned header element is the **current date**, not a static title: format `DD MMM (DDD)` (e.g. ` 9 Aug (Sun)`) — English month/weekday abbreviations (`Aug`, `Sun`) from the default C locale.
- **Single-digit days**: a space occupies the tens slot (` 9`, not `09`) so the ones digit stays at a stable x-position as the date increments. Built in `main.c` `build_header_date_str()` (day via `snprintf`, month+weekday via `strftime("%b (%a)")`) — `%e` is not relied upon.
- **Font**: `u8g2_font_helvR10_tr` (10 px regular), drawn at (14, 24) in `render_header()`.
- **Pre-sync placeholder**: before the SNTP clock is valid (`now < EPOCH_SYNC_THRESHOLD`, 1700000000 = 2023-11-14), the slot shows `-- --- (---)` instead of a 1970-era date. Same threshold constant used by `ntp_wait_for_sync()`.
- **Midnight rollover**: automatic — the header re-renders every refresh interval and re-reads `localtime()`.
- **Weather anchoring**: temperature/humidity are positioned 16 px right of the date label's measured width (`x_temp = 14 + w_date + 16`), so the weather block shifts a few px day-to-day as the date width varies. The time element is right-anchored and never moves.

## Weather Temperature & Humidity
- **Source**: HKO rhrread API (`https://data.weather.gov.hk/weatherAPI/opendata/weather.php?dataType=rhrread&lang=en`) — one fetch supplies both `temperature.data[]` and `humidity.data[]`. `humidity.data` has fewer stations than `temperature.data`; never assume a station present in one is present in the other.
- **Station**: Configurable via `routes.json` `"weather"."station"` (default: `"Hong Kong Observatory"`) — the same station governs both temperature and humidity (no separate humidity key).
- **Fetch model**: Piggyback on `eta_fetch_task` (every 20th cycle, ~10 min). Counter starts at 20 so first fetch runs immediately on boot — no 10-min wait for initial weather.
- **Font**: `u8g2_font_profont22_mf` (22 px bold) for **both** temperature and humidity, same baseline as the date label (y=24), 16 px gap from the date label, 12 px gap between temperature and humidity.
- **Format**: `NN°C` (e.g. `28°C`, `\xC2\xB0` for `°`) then `NN%` (e.g. `70%`) rendered immediately after.
- **Stale TTL**: 30 minutes (1800 s), shared by both fields. Hidden entirely when stale or unavailable — no placeholder, no `--°C`, no `--%`.
- **Overlap drop order**: If both fit (`x_temp + w_temp + 12 + w_hum + 8 <= x_time_left`), draw both. Else if temperature alone fits (`x_temp + w_temp + 8 <= x_time_left`), drop humidity and draw temperature only. Else omit the whole weather block for that frame. Time always wins.
- **Humidity availability**: If the last successful fetch found no humidity entry for the configured station (but temperature was found), humidity hides independently — no last-known-good retention (temperature still shows alone). If temperature is missing, both hide (shared 30-min TTL).
- **Data model**: `weather_t` struct with spinlock (mirrors battery.c pattern). Fields: `temp_c` (`float`), `humidity_pct` (`int`, 0–100), `humidity_valid` (`bool`), `last_updated_epoch` (`time_t`), `valid` (`bool`). One shared `last_updated_epoch` so the two fields can never diverge. Writer: `eta_fetch_task` → `weather_fetch_once()`. Reader: `display_task` → `weather_snapshot()`/`weather_get_temp_str()`/`weather_get_humidity_str()`.
- **Files**: `main/weather_hko.c`, `main/weather_hko.h`, `main/http_util.c`, `main/http_util.h`
- **Failure behaviour**: HTTP error → log warning, preserve last-known-good. JSON parse failure → log warning, preserve last-known-good. Station not found in temperature → log warning, no update. Station not found in humidity only → log warning, hide humidity for this update. After 30 min of no successful fetch, both disappear from header.
- **HKO API response shape differs from KMB/Citybus** — never assume interchangeability.

## WiFi Connection State
- **Spinlock-protected state**: `s_wifi_connected` (`bool`) guarded by `s_wifi_lock` (`portMUX_TYPE`). Writer: `wifi_event_handler()` in `main.c`. Reader: `display_task()`.
- **Display behaviour**: When WiFi is disconnected (or never connected), the footer band replaces `"Updated HH:MM:SS"` with `"Connecting..."`. This covers both the boot-time "never connected" case and the runtime "disconnected and reconnecting" case with a single string.
- **State transitions**:
  - `WIFI_EVENT_STA_DISCONNECTED` → `s_wifi_connected = false` (before `esp_wifi_connect()`)
  - `IP_EVENT_STA_GOT_IP` → `s_wifi_connected = true` (before `xEventGroupSetBits()`)
- **Stale ETA behaviour**: During disconnection, `eta_fetch_task` preserves last-known-good ETAs for up to 180 s, then expires to `"--"`. The footer shows "Connecting..." throughout.
- **No new files**: The state variable is `static` in `main.c`, declared adjacent to the existing globals. No new header.

## Multi-Page Display
- **JSON schema**: `routes.json` uses a top-level `"pages"` array, each page object contains a `"routes"` array. Legacy top-level `"routes"` is accepted as a single-page fallback.
- **Fetch strategy**: Only the visible page's routes are fetched. `eta_fetch_task` reads `s_active_page` once per cycle and fetches only that page's routes.
- **Page toggle**: KEY button (GPIO18) toggles `s_active_page` (0↔1). `display_task` calls `button_consume_presses()` each tick. If `s_page_count > 1` and a press is detected, the page toggles and `xTaskNotifyGive` wakes `eta_fetch_task` for an immediate fetch.
- **Footer indicator**: "Page X/Y" rendered 10 px after "Updated HH:MM:SS" in the footer band by `render_footer()`. Hidden when `s_page_count == 1`.
- **Button driver**: `main/button.c` — GPIO18 falling-edge ISR, atomic press counter. `button_consume_presses()` returns count since last call and resets to 0.
- **GPIO18 ownership**: Page-toggle owns short-press. The pending sleep plan (`docs/plan-display-sleep-button-wake.md`) must be reworked (long-press discriminator or different button) when implemented.
- **Double-buffer**: `s_route_buf[2][MAX_PAGES][ROUTES_PER_PAGE]` — 2 buffers × 2 pages × 3 routes. `s_active_buf_idx` and `s_active_page` are atomically-swapped `volatile int` (word-sized, no tearing on ESP32-S3).
- **Partial pages**: If a page has fewer than 3 routes, remaining rows render blank (no divider, no text, no ETA). Controlled by `route_count` parameter in `render_dashboard()`.
- **Page 2 optional**: If `s_page_count == 1` (legacy JSON or single page), button press is a no-op and footer hides the page indicator.

