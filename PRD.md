# PRD: hk-bus-eta-rlcd

## 1. Project Goal

Build a dedicated, wall-mountable Hong Kong bus ETA display on the Waveshare ESP32-S3-RLCD 4.2" board. The device fetches real-time arrival estimates from KMB and Citybus open APIs for exactly three fixed routes, renders them on the reflective ST7305 display with large, legible text, and refreshes the display every 15 seconds (on :00/:15/:30/:45 boundaries) while fetching ETA data every ~30 seconds (jittered). No touch, no audio, no OTA — a single-purpose appliance that shows "when's the next bus" at a glance.

## 2. Hardware Constraints

| Aspect | Constraint |
|---|---|
| **SoC** | ESP32-S3 (dual-core LX7 @ 240 MHz), 512 KB SRAM, 16 MB Flash, 8 MB PSRAM |
| **Display** | ST7305 reflective monochrome controller, 400 × 300 px (landscape orientation), 1-bit per pixel |
| **Display interface** | SPI — confirmed pin mapping per [docs/waveshare-pinout.md](file:///Users/alanlee/Documents/trae_projects/hkbus-eta-rlcd/docs/waveshare-pinout.md): CLK = GPIO11, MOSI = GPIO12, CS = GPIO40, DC = GPIO5, RST = GPIO41. MISO is unused (half-duplex write-only). |
| **Display driver init/timing** | Init sequence derived from the **Waveshare official reference driver** (`../waveshare-reference/02_Example/ESP-IDF/11_U8G2_Test/components/u8g2_st7305/`), which is the authoritative source for this exact board. Do NOT use values from the generic ST7305 datasheet §7.9.2 — the Waveshare values differ significantly (voltage trims, frame rate, duty setting, gate timing, pixel format, display inversion). The register map is documented in CLAUDE.md §ST7305 Register Values — Starting Point from Waveshare Reference. |
| **Text / font rendering** | U8g2 library in *page-buffer mode* (no full framebuffer allocation). zh-HK CJK font support **working end-to-end** — destination and bus-stop name fields render zh-HK Chinese (e.g. 黃埔花園, 海濱花園總站). `route_config.c` reads `dest_zh`/`stop_zh` from `routes.json` (falls back to `dest_en`/`stop_en` if absent). See CLAUDE.md §zh-HK CJK Font Support for font specs (file names, glyph coverage, partition layout). The 8 MB PSRAM is *available* but the software shall not *depend* on PSRAM for core function; PSRAM may be used optionally for font caching if needed, but must gracefully degrade if PSRAM init fails. |
| **GUI framework** | **No LVGL or any other full-featured GUI framework.** Text rendering is handled directly via U8g2 primitives. |
| **Wireless** | Wi-Fi 4 (802.11n, 2.4 GHz), built-in ceramic antenna. BLE unused in this version. |
| **Power** | Primary: battery (fixed installation, not portable; USB Type-C present for charging and firmware flashing). Active power-saving: Wi-Fi modem-sleep (`WIFI_PS_MIN_MODEM`) between fetch cycles. Deep-sleep intentionally NOT used — the device maintains a persistent Wi-Fi connection, with ETA fetch every ~30 s and display render every 15 s. Tier 2/3 deferred (see §10). |
| **RTC** | Onboard PCF85063 RTC chip present but not required for core ETA display (SNTP suffices). May be leveraged in a future revision to reduce SNTP polling. |
| **MicroSD** | Slot present on board but left **unused** in this version. |
| **Audio / mic** | ES7210 + ES8311, dual-mic array — entirely unused in this version. |

## 3. Functional Requirements

1. **Display initialisation** — On boot, initialise the ST7305 over SPI with the confirmed pin mapping (GPIO 11/12/40/5/41). Flash a solid test pattern (all-pixels-on) for 500 ms to visually confirm the display is alive before proceeding to the dashboard.
2. **Wi-Fi connection** — Connect to a pre-configured Wi-Fi network. Credentials are stored in a config file (not hardcoded). The board retains credentials across reboots.
3. **SNTP time sync** — After obtaining an IP, synchronise the system clock via NTP. Use the HK timezone (UTC+8). The NTP server is **stdtime.gov.hk**. Display "HH:MM" in the dashboard header once sync completes. A daily NTP resync is triggered at 06:00 local time to correct clock drift accumulated over 24+ hours of uptime (see FR 12).
4. **Load route configuration** — On startup, read `routes.json` from the SPIFFS partition. The file defines exactly 3 routes (a mix of KMB and Citybus), each with: operator, stop ID, route number, destination label (zh-HK via `dest_zh`, with `dest_en` fallback), and bus-stop name (zh-HK via `stop_zh`, with `stop_en` fallback). If the file is missing or corrupt, show a persistent error message on the display and halt further ETA fetching.
5. **Fetch KMB ETA** — For each KMB route, HTTP GET the KMB route-specific ETA endpoint. Parse the earliest `eta` and store the raw ISO 8601 timestamp as a `time_t` epoch value — do not precompute minutes at fetch time. When the stop is a terminal where the API returns both directions, filter to keep only the configured destination (case-insensitive `dest_en` comparison). Implementation: see CLAUDE.md §Data Source Handling and §Permanent Never List (Direction filtering).
6. **Fetch Citybus ETA** — For each Citybus route, HTTP GET `https://rt.data.gov.hk/v2/transport/citybus/eta/CTB/{stop_id}/{route}`. The response already contains only the requested route. Parse the earliest `eta` timestamp and store the raw ISO 8601 timestamp as a `time_t` epoch value — do not precompute minutes at fetch time.
7. **Render dashboard** — On wall-clock boundaries (15 s interval), draw the following zones on the 400 × 300 display (decoupled from the ETA fetch cycle — always renders the last known ETA values). Current boundary times and refresh cadence: see CLAUDE.md §Display Refresh. Minutes-remaining are computed fresh at render time from the stored raw epoch timestamps and the current wall-clock time — never from precomputed values stored at fetch time:
    - **Header** (top ~8%): Current local time in `HH:MM` format, large font.
    - **Route rows** (middle ~80%): Up to 3 rows, each showing:
        - Route number (e.g. "1A"), medium-bold font
        - Destination (zh-HK e.g. "黃埔花園"), same row, lighter style
        - ETA in minutes (e.g. "5 min" or "--" if unavailable), right-aligned
    - **Footer** (bottom ~10%): "Updated HH:MM:SS" on the left, "Battery: XX%" (battery percentage) on the right.
    - **Reconnecting banner**: If Wi-Fi is lost mid-operation, overlay or insert a "Reconnecting..." banner row immediately below the header. Keep the last known ETA values on screen but visually greyed out (e.g. inverse/dimmed style). The banner persists until Wi-Fi reconnects — no auto-dismiss.
8. **Error handling per operator** — If one operator's API fails (network error, HTTP non-200, timeout) while the other succeeds, the last-known-good ETA values for the failed operator's routes are preserved on screen for up to 3 minutes (measured from the ETA timestamp, not the fetch time). If the stale ETA is more than 3 minutes in the past, it expires to "--". The working operator's routes fetch and display normally. Do *not* halt the entire refresh cycle.
9. **Refresh cadence** — The ETA fetch cycle and the display render cycle are **decoupled**. The display re-renders on wall-clock boundaries (15 s interval) so the header clock stays precisely aligned regardless of network/HTTP fetch latency. The ETA fetch runs on a longer cadence (~30 s, jittered). Always render the last known ETA values — never block the render cycle waiting for a fresh fetch. Current values: see CLAUDE.md §Display Refresh.
10. **Boot-to-display target** — From power-on to the first fully rendered ETA dashboard, the device should take no more than **10 seconds**. If Wi-Fi or SNTP is slow, show the partial state (time only or "Connecting..." message) within 10 s rather than a blank screen.
11. **Null ETA display** — If a route has no available ETA (API returns no data, or the next bus is beyond the API's reporting horizon), display "--" for that route. Never display "0" as a substitute for "no data".
12. **Daily NTP resync** — Once per day at 06:00 local time, trigger a one-shot SNTP resync to correct clock drift accumulated over 24+ hours of uptime. The resync is blocking but bounded (10 s max retry); if it times out, log a warning and continue normally — do not crash or hang. The "once per day" guard uses day-of-year tracking so the resync fires exactly once per day, with one retry attempt per day regardless of prior success/failure.

## 4. Data Source

### KMB (九巴)

| Field | Value |
|---|---|
| Base URL | `https://data.etabus.gov.hk/v1/transport/kmb/eta/{stop_id}/{route}/1` |
| Method | HTTP GET |
| Response format | JSON — top-level key `"data"` contains an array. Each element includes: |
| Key fields used | `route` (string), `eta_seq` (int, used to pick the earliest), `eta` (ISO 8601 timestamp), `dir` (direction) |
| Filtering | Server-side: the route-specific `/eta/` endpoint returns only the requested route. Client-side: pick lowest `eta_seq`. At terminal stops, also filter by `dest_en` (case-insensitive) to exclude the opposite direction. |
| Rate limit | Unknown — see **Needs Clarification #4**. |

### Citybus (城巴)

| Field | Value |
|---|---|
| Base URL | `https://rt.data.gov.hk/v2/transport/citybus/eta/CTB/{stop_id}/{route}` |
| Method | HTTP GET |
| Response format | JSON — top-level key `"data"` contains an array. Each element includes: |
| Key fields used | `route` (string), `eta` (ISO 8601 timestamp), `dir` (direction) |
| Filtering | Already route-filtered server-side; pick earliest `eta` entry. |
| Rate limit | Unknown — see **Needs Clarification #4**. |

**Key difference**: Both KMB and Citybus now use route-specific endpoints. KMB uses `/eta/{stop_id}/{route}/{service_type}`; Citybus uses `/eta/CTB/{stop_id}/{route}`. The response shapes still differ in field naming and structure — parsing logic must not assume interchangeability.

## 5. Display Requirements

- **Layout zones**: See Section 3, requirement 7 (Render dashboard) for zone definitions and proportions.
- **Readability targets**:
    - Route number text: minimum 24 px glyph height for at-a-glance readability from ~1 m distance.
    - ETA value text: minimum 24 px glyph height.
    - Destination / footer text: minimum 16 px glyph height.
- **zh-HK font handling (WORKING — verified on device)**: Destination and bus-stop names render in zh-HK Chinese. ASCII fallback retained for pure-ASCII strings. See CLAUDE.md §zh-HK CJK Font Support for implementation details (font files, glyph coverage, partition table).
- **U8g2 mode**: Page-buffer mode (not full framebuffer). The U8g2
  page buffer is sized to a small fraction of the total display to
  keep SRAM usage low.
- **No stock U8g2 constructor exists for 400×300**. The **Waveshare
  official `u8g2_st7305` component** (at `../waveshare-reference/
  02_Example/ESP-IDF/11_U8G2_Test/components/u8g2_st7305/`) is the
  authoritative starting point. It uses a custom DRAW_TILE callback
  with a 4×4 lookup table, `u8g2_ll_hvline_vertical_top_lsb` pixel
  layout, and `U8G2_R1` rotation. Our project should adopt this
  driver as the baseline, adapting it to our landscape-optimised
  layout if needed.
  - Landscape orientation is set via U8g2 rotation parameter
    (U8G2_R0 / U8G2_R1, etc.) once a working constructor exists.
- **PSRAM availability**: 8 MB PSRAM is present. The Waveshare
  reference driver allocates the display buffer from PSRAM via
  `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`. The PRD requirement
  to "operate without PSRAM" remains, but PSRAM may be used for
  buffer allocation if internal SRAM is insufficient.
- **Design reference**: Once `design.md` exists, the layout shall conform to the zone definitions and visual rules defined there. This PRD defines *what* to display; `design.md` defines *where* and *how it looks*.

## 6. Architecture

The firmware runs two independent FreeRTOS tasks after boot initialisation:

- **`eta_fetch_task`** (priority: `tskIDLE_PRIORITY+2`): Owns the ETA fetch loop and Wi-Fi modem-sleep toggling. Cadence specified in CLAUDE.md §Display Refresh (current: ~30 s ±10% jitter). Writes fetched ETA values into the **inactive** double-buffer, then atomically flips a shared active-buffer index.
- **`display_task`** (priority: `tskIDLE_PRIORITY+3`): Owns wall-clock render boundary alignment, the daily NTP resync, and `render_dashboard()`. It always reads from the **active** double-buffer and never waits for fresh data. Render boundaries specified in CLAUDE.md §Display Refresh.

### Shared data: double-buffering (no mutex)

Two full `route_data_t[3]` buffers (`s_route_buf[2][3]`) plus an atomically-swapped active-buffer index (`s_active_buf_idx`, 0 or 1). No mutex or semaphore is used:

- The active index is a word-sized `int` — aligned 32-bit writes on ESP32-S3 are naturally atomic (no tearing).
- `display_task` reads the index once per render cycle, then reads from that buffer throughout. A concurrent flip by `fetch_task` only affects the *next* render cycle.
- `fetch_task` writes the inactive buffer while `display_task` reads the active buffer — no reader/writer contention on the same buffer at the same time.

### Task ownership

| Concern | Owner |
|---|---|
| ETA fetch loop (HTTP, JSON parsing) | `eta_fetch_task` |
| Wi-Fi modem-sleep toggling (`WIFI_PS_NONE`/`WIFI_PS_MIN_MODEM`) | `eta_fetch_task` |
| Wall-clock `:00`/`:15`/`:30`/`:45` alignment | `display_task` |
| Daily NTP resync at 06:00 | `display_task` |
| `render_dashboard()` and all render helpers | `display_task` |

## 7. Non-Functional Requirements

| Requirement | Specification |
|---|---|
| **Refresh interval** | ETA fetch and display render are decoupled. Display re-renders on wall-clock boundaries (15 s interval) so the header clock stays aligned. ETA fetch runs ~30 s ±10% jitter. Never block the render cycle waiting for a fetch — always render the last known ETA values. Current values: see CLAUDE.md §Display Refresh. |
| **Boot time** | ≤ 10 s from power-on to first ETA dashboard render. If network is slow, show partial state (e.g. "Connecting...") within 10 s. |
| **Wi-Fi reconnect** | On disconnect: show "Reconnecting..." banner immediately, retain last known ETA greyed out. Reconnect automatically (infinite retry, 5 s backoff between attempts). Banner persists until successfully reconnected — no auto-dismiss. |
| **SNTP failure** | If SNTP sync fails or is delayed beyond boot window, the dashboard shows "HH:MM" as "----" (unsynced indicator). Normal time display resumes once sync completes on the next refresh cycle. |
| **Power / battery** | Battery-powered (fixed install, not portable). Active power-saving: Wi-Fi modem-sleep (`WIFI_PS_MIN_MODEM`) is enabled between fetch cycles to reduce radio power while keeping the connection alive. The full display buffer is re-sent every render cycle (no partial-window optimization). Deep-sleep is NOT used — the device stays connected to Wi-Fi. Display render cadence: see CLAUDE.md §Display Refresh. Full battery optimisation (Tier 2/3) is deferred — see §10. |
| **Firmware update** | USB reflash via `idf.py flash` only. No OTA updates. |
| **Memory** | Must operate correctly without relying on PSRAM. PSRAM may be used as an optional cache but the core display and ETA pipeline must function with only internal SRAM (512 KB). |
| **Time source** | NTP only, server: `stdtime.gov.hk`. Timezone: UTC+8 (HKT). No RTC fallback in this version. Daily NTP resync at 06:00 local time to correct clock drift (see §3 FR 12). |

## 8. Out of Scope

The following are explicitly **not** part of this version:

- Runtime route switching via buttons, touch, or any UI interaction
- MTR (港鐵), GMB (綠色專線小巴), or any transport operator beyond KMB and Citybus
- Support for more than 3 simultaneous routes
- OTA firmware updates
- MicroSD card usage (slot present, software ignores it entirely)
- Display contrast or brightness runtime adjustment
- Audio playback, microphone recording, or voice interaction
- RTC-based timekeeping (SNTP only; RTC chip present but unused)
- Battery-powered optimisation, deep sleep, or power management states
- BLE connectivity or smartphone companion app
- Web configuration portal (Wi-Fi credentials stored in config file, not setup via captive portal)
- Over-the-air configuration of routes or stops
- Historical data, graphs, or prediction algorithms beyond the raw API ETA
- Multi-language UI (zh-HK CJK font support working end-to-end for destinations and bus-stop names via custom `bdfconv`-generated fonts; ASCII fallback retained for pure-ASCII strings)

## 9. Definition of Done

A feature or fix is considered **Done** when:

### Code Review Checklist

- [ ] Naming conventions are consistent across all files (functions, types, macros, files)
- [ ] Every ESP-IDF function call that returns `esp_err_t` is checked; errors are logged and handled (not silently ignored)
- [ ] No hardcoded secrets or credentials in source code — Wi-Fi credentials are read from a config file (Kconfig or SPIFFS)
- [ ] No unbounded string/array operations — all buffers have explicit sizes; `snprintf` / `strncpy` are used instead of `sprintf` / `strcpy`
- [ ] Code matches requirements documented in PRD.md and layout matches design.md

### Self-Test Checklist (to be run after every implementation step)

- [ ] Re-read every new or modified file against PRD.md and design.md
- [ ] Explicitly note any mismatch between implementation and requirements before presenting the result
- [ ] If a mismatch exists, either fix it or flag it for discussion — do not silently proceed

### Closing Rule

At the end of every future implementation step, ask:

> *"Is there anything in this requirement that's unclear or unspecified? If so, ask before proceeding."*

## 10. Resolved Decisions

The following were previously flagged as open questions and have been resolved as follows:

| # | Question | Decision |
|---|---|---|
| 1 | ST7305 init sequence | **Waveshare official reference driver** (`../waveshare-reference/02_Example/ESP-IDF/11_U8G2_Test/components/u8g2_st7305/`) is the authoritative source. Do not use generic datasheet §7.9.2 values. Register map documented in CLAUDE.md. |
| 2 | U8g2 back-end | Use the **Waveshare `u8g2_st7305` component** as the baseline (custom DRAW_TILE callback, LUT-based pixel layout, `u8g2_ll_hvline_vertical_top_lsb`, `U8G2_R1` rotation). Adapt to landscape-optimised layout if needed. |
| 3 | Countdown meaning | End-of-cycle countdown: counts down to the next render boundary. Reset value depends on current refresh cadence — see CLAUDE.md §Display Refresh. |
| 4 | API rate limits | 200–500 ms fixed delay between sequential calls as defensive courtesy. Exponential backoff added only if HTTP 429 observed in testing — not over-engineered preemptively. |
| 5 | WiFi credentials storage | Kconfig.projbuild via `idf.py menuconfig`, stored in sdkconfig (not committed to git). Routes/stops in routes.json on SPIFFS (runtime data, may change without reflash). These are intentionally separate mechanisms. |
| 6 | Error display duration | Two distinct cases: (a) Wi-Fi down → "Reconnecting..." banner + greyed-out ETA until reconnected; (b) API call fails while Wi-Fi is up → "--" for that route per-cycle, no persistent banner, retries next cycle. No third visual state. |
| 7 | **Time-based display voltage/contrast mode** | **Removed** | Originally implemented as two-state voltage-profile switching (high-contrast 06:00–10:00, low-voltage other hours). **Removed 2026-07-20**: The voltage deltas (~7–9% of drive voltage) are below the visible threshold for the ST7305 reflective LCD and do not meaningfully reduce power or glare. The code (`u8g2_st7305_set_voltage_profile()`, two profile arrays, and the hour-check in `display_task`) has been deleted. Init sequence baseline values are unchanged. |
| 8 | **Larger zh-HK fonts (dest + stop)** | **TBC — deferred.** See `docs/plan-larger-fonts-noto-otf2bdf.md` for the full plan (rasterise Noto Sans HK via otf2bdf at 24px/20px, resize factory partition to 8 MB). Not started yet. |

### Open / TBC Decisions

The following have been identified as future scope items and deliberately deferred. They are tracked here so that future sessions know they were considered and not overlooked.

| # | Item | Status | Notes |
|---|---|---|---|
| 1 | Battery percentage indicator | **Implemented** | Footer now shows `Battery: XX%` (replacing `Next: Xs`). ADC1 on GPIO4, curve-fitting calibration, 3× divider, piecewise linear LUT (11-point Li-ion discharge curve) with hysteresis (1% deadband) to prevent display flicker. **Voltage sampled during confirmed Wi-Fi-idle windows** (after `WIFI_PS_MIN_MODEM` re-enable, 50 ms settle delay) to avoid Wi-Fi TX load sag causing false discharge readings. Filter pipeline: rolling 5-sample median → EMA (α=0.2) → LUT → two-reading confirmation for jumps >6 points. `battery_sample_if_due()` called from `eta_fetch_task` every 2nd cycle (~60 s); `battery_get_percentage()` returns cached value. Plan file removed after implementation. **Known simplification**: Uses a generic 18650 Li-ion discharge curve — not calibrated to the specific cell/load in this device. A per-unit calibration step (e.g. measuring the actual cell's open-circuit voltage at known charge levels) could improve accuracy further. |
| 2 | Larger zh-HK fonts (dest + stop) | TBC (deferred) | See `docs/plan-larger-fonts-noto-otf2bdf.md`. |
| 3 | Temperature/humidity display | Future | — |
| 4 | Deep-sleep with periodic wake | Future | — |
| 5 | Display sleep + button wake | **Pending** | Proposed feature: display off (0x28 sleep) outside a configurable morning window; button-triggered wake (on-board KEY button, GPIO18) for a configurable timeout. Full plan: `docs/plan-display-sleep-button-wake.md`. This is a new feature independent of the now-removed voltage-profile code (Decision #7). |