# Plan: Add HKO Temperature to Header Band

> **Revised** — incorporates codebase review, design.md updates, and all
> agreed decisions. Supersedes the original draft.

---

## 0. Decisions Summary

| Decision | Value | Rationale |
|----------|-------|-----------|
| Fetch model | Piggyback on `eta_fetch_task` (every 20th cycle, ~10 min) | Avoids Wi-Fi PS race from a separate task; no new task/stack |
| Station | `Hong Kong Observatory` (configurable via `routes.json`) | Always available in API; user confirmed |
| Temperature font | `u8g2_font_profont22_mf` (22 px bold) | 10→22→32 hierarchy; temperature reads as data, not title |
| Vertical position | Same baseline as title (y=24) | Reads as one mixed-size line; temperature towers above title |
| Gap from title | 16 px | Between 12 px padding rhythm and 20 px plan draft |
| Stale TTL | 30 minutes | Weather changes slowly; 3 consecutive failed fetches before hide |
| Failure behaviour | Hide entirely (no placeholder) | Clean header with just title + time |
| Degree symbol | Test on device; fallback to `C` if glyph missing | `profont22_mf` is `_mf` (0x20–0xFF), `°` (0xB0) should be present |
| Counter advance | Every cycle (including ETA fetch failures) | Predictable 10-min interval; weather fetch runs in same Wi-Fi window |

---

## 1. Visual / Layout Specification

Header band is 36 px tall, full-width black, three text elements:

```text
[ HK Bus ETA ][ 16px gap ][ 28°C (22px bold)      ...space...      ][ 14:32 (32px bold) ]
```

Current rendering (from [display.c:84-103](file:///Users/alanlee/Documents/trae_projects/hkbus-eta-rlcd/main/display.c#L84-L103)):
- Title `"HK Bus ETA"`: `u8g2_font_helvR10_tr` (~10 px), drawn at `(14, 24)`
- Time `HH:MM`: `u8g2_font_logisoso32_tf` (32 px), right-anchored at `DISP_WIDTH - 14`

New temperature element:
- Font: `u8g2_font_profont22_mf` (22 px bold — same font as route numbers)
- Position: `x_temp = x_title + w_title + 16`, where `x_title = 14`
- Baseline: `y = 24` (same as title)
- Format: `"NN°C"` (e.g. `"28°C"`), using `\xC2\xB0` for `°`
- Overlap guard: if `x_temp + w_temp + 8 > x_time_left`, skip drawing
  (where `x_time_left` is the left edge of the time string)
- Failure case: draw nothing (caller passes `NULL` for temp string)

Vertical math (22 px font at y=24 baseline):
- Ascent ~18, descent ~4 → glyphs span y=6 to y=28
- Time at y=31 baseline → glyphs span y=5 to y=36
- Both within 36 px band; horizontally separated, no collision

---

## 2. Data Model

Single-struct shared state with spinlock (mirrors [battery.c](file:///Users/alanlee/Documents/trae_projects/hkbus-eta-rlcd/main/battery.c) pattern — same task relationship: `eta_fetch_task` writes, `display_task` reads):

```c
typedef struct {
    float  temp_c;
    time_t last_updated_epoch;
    bool   valid;
} weather_temp_t;
```

Storage in `weather_hko.c`:
```c
static weather_temp_t s_weather;
static portMUX_TYPE s_weather_lock = portMUX_INITIALIZER_UNLOCKED;
```

- Writer (`weather_fetch_once`, called from `eta_fetch_task`):
  - On success: `taskENTER_CRITICAL` → write `s_weather` → `taskEXIT_CRITICAL`
  - On failure: do not modify `s_weather` (last-known-good preserved)
- Reader (`weather_snapshot`, called from `display_task`):
  - `taskENTER_CRITICAL` → copy `s_weather` locally → `taskEXIT_CRITICAL`
  - Apply TTL: if `!valid` or `now - last_updated_epoch > 1800` (30 min), return `false`

No double-buffer needed — the data is ~16 bytes and a spinlock copy is trivial.

---

## 3. Fetch Integration: Piggyback on `eta_fetch_task`

No new task. Weather fetch runs inside `eta_fetch_task`'s existing `WIFI_PS_NONE` window, after ETA fetches, before `WIFI_PS_MIN_MODEM` re-enable.

In [main.c](file:///Users/alanlee/Documents/trae_projects/hkbus-eta-rlcd/main/main.c) `eta_fetch_task()`, after the ETA fetch loop and before `esp_wifi_set_ps(WIFI_PS_MIN_MODEM)`:

```c
/* Weather fetch every 20th cycle (~10 min at 30 s/cycle).
 * s_weather_cycle starts at 20 so the first cycle fetches immediately
 * on boot — no 10-min wait for initial temperature. */
static int s_weather_cycle = 20;
if (++s_weather_cycle >= 20) {
    s_weather_cycle = 0;
    weather_fetch_once();  /* HTTP + parse + update s_weather */
}
```

The counter advances on every cycle regardless of ETA fetch success (user decision). After the initial fetch, subsequent fetches occur every 20th cycle (~10 min).

---

## 4. HKO API Fetch and JSON Parsing

### 4.1 HTTP Helper Refactor

`http_get_body()` in [eta_fetcher.c:115](file:///Users/alanlee/Documents/trae_projects/hkbus-eta-rlcd/main/eta_fetcher.c#L115) is `static` and takes ETA-specific logging params. Extract to a shared module:

**New files**: `main/http_util.c`, `main/http_util.h`

```c
/* http_util.h */
char *http_get_body(const char *url, const char *log_tag);
```

- Moves `http_event_handler`, `body_capture_t`, `http_get_body` from `eta_fetcher.c`
- `log_tag` replaces the `operator`/`route` params (caller passes e.g. `"eta kmb 30X"` or `"weather"`)
- `eta_fetcher.c` includes `http_util.h` and calls `http_get_body(url, log_buf)` where `log_buf` is formatted locally
- Update `main/CMakeLists.txt` to include `http_util.c`

### 4.2 Weather Module

**New files**: `main/weather_hko.c`, `main/weather_hko.h`

```c
/* weather_hko.h */
void weather_init(const char *station_name);
void weather_fetch_once(void);
bool weather_snapshot(weather_temp_t *out);  /* returns false if invalid/stale */
bool weather_get_temp_str(char *buf, size_t len);  /* convenience: fills "NN°C", returns false if unavailable */
```

**Endpoint**: `https://data.weather.gov.hk/weatherAPI/opendata/weather.php?dataType=rhrread&lang=en`

**JSON structure** (relevant part):
- `temperature`: object
- `temperature.data`: array of station readings
- Each entry: `place` (string), `value` (number), `unit` (string, e.g. `"C"`)

**Parse logic**:
1. `http_get_body(HKO_URL, "weather")` → body or NULL
2. `cJSON_ParseWithLength(body, body_len)`
3. Navigate `root → "temperature" → "data"` (array)
4. Iterate entries, `strcasecmp(place, s_station_name)` to find match
5. Validate `unit == "C"`
6. On match: write `temp_c`, `last_updated_epoch = time(NULL)`, `valid = true` under spinlock
7. On no match / parse failure / HTTP error: log warning, do not modify `s_weather`

### 4.3 Station Configuration

In [routes.json](file:///Users/alanlee/Documents/trae_projects/hkbus-eta-rlcd/spiffs_data/routes.json), add top-level `"weather"` block:

```json
{
  "routes": [ ... ],
  "refresh_seconds": 10,
  "weather": {
    "station": "Hong Kong Observatory"
  }
}
```

In [route_config.c](file:///Users/alanlee/Documents/trae_projects/hkbus-eta-rlcd/main/route_config.c):
- Parse `"weather"."station"` into a static buffer (default `"Hong Kong Observatory"` if absent)
- Add accessor: `const char *route_config_get_weather_station(void)`

In `app_main()` ([main.c:438](file:///Users/alanlee/Documents/trae_projects/hkbus-eta-rlcd/main/main.c#L438)):
- After `route_config_load()`, call `weather_init(route_config_get_weather_station())`

---

## 5. Rendering Code Changes

### 5.1 Signature Changes

Current ([display.h:57](file:///Users/alanlee/Documents/trae_projects/hkbus-eta-rlcd/main/display.h#L57)):
```c
void render_header(const char *time_str);
void render_dashboard(const char *time_str, const char *updated_str,
                      int battery_pct, const route_data_t routes[3]);
```

New:
```c
void render_header(const char *time_str, const char *temp_str);
void render_dashboard(const char *time_str, const char *temp_str,
                      const char *updated_str, int battery_pct,
                      const route_data_t routes[3]);
```

`temp_str` is either `NULL` (no temperature) or a pre-formatted `"NN°C"` string.

### 5.2 `render_header` Changes

In [display.c:84](file:///Users/alanlee/Documents/trae_projects/hkbus-eta-rlcd/main/display.c#L84), after drawing the title and before drawing the time:

```c
/* Temperature (22px bold), 16px right of title, same baseline */
if (temp_str) {
    u8g2_SetFont(u, u8g2_font_profont22_mf);
    int w_title = u8g2_GetStrWidth(u, "HK Bus ETA");  /* measured in title font */
    /* Re-measure title in its own font for correct width */
    u8g2_SetFont(u, u8g2_font_helvR10_tr);
    w_title = u8g2_GetStrWidth(u, "HK Bus ETA");

    u8g2_SetFont(u, u8g2_font_profont22_mf);
    int w_temp = u8g2_GetUTF8Width(u, temp_str);
    int x_temp = 14 + w_title + 16;

    /* Time left edge (time is right-anchored at DISP_WIDTH - 14) */
    u8g2_SetFont(u, u8g2_font_logisoso32_tf);
    int w_time = u8g2_GetStrWidth(u, time_str);
    int x_time_left = DISP_WIDTH - 14 - w_time;

    if (x_temp + w_temp + 8 <= x_time_left) {
        u8g2_SetFont(u, u8g2_font_profont22_mf);
        u8g2_SetDrawColor(u, 0);  /* white-on-black */
        u8g2_DrawUTF8(u, x_temp, 24, temp_str);
    }
    /* If overlap would occur, skip — temperature omitted for this frame */
}
```

### 5.3 `display_task` Changes

In [main.c](file:///Users/alanlee/Documents/trae_projects/hkbus-eta-rlcd/main/main.c) `display_task()`, before `render_dashboard()`:

```c
char temp_str[8] = {0};
const char *temp_ptr = NULL;
if (weather_get_temp_str(temp_str, sizeof(temp_str))) {
    temp_ptr = temp_str;
}

render_dashboard(time_str, temp_ptr, last_updated, battery_pct,
                 s_route_buf[buf_idx]);
```

### 5.4 Degree Symbol Handling

`profont22_mf` is an `_mf` (medium font) covering 0x20–0xFF. `°` (U+00B0, encoded as `\xC2\xB0` in UTF-8) should be present.

**Verification**: During on-device testing, check that `"28°C"` renders the degree symbol. If it renders as a blank/box, fall back to `"28C"` (no degree) by changing the format string in `weather_get_temp_str()`. This is a one-line change — no architectural impact.

---

## 6. Documentation Updates

### 6.1 design.md (DONE)

- [x] Typography table: added "Header temperature (NN°C) | 22 px | Bold | u8g2_font_profont22_mf"
- [x] ASCII art: header line now shows `HK Bus ETA  28°C  ...  14:32`
- [x] Structural rule 1: updated from "two text elements" to "three text elements" with temperature spec
- [x] Checklist: updated header verification item
- [x] New §8: External Data — Temperature (source, station, cadence, TTL, format, failure behaviour)

### 6.2 PRD.md

- Update §10 Open/TBC #3 ("Temperature/humidity display"): status → **Implemented**
- Add FR #13: "Display current air temperature in the header band using HKO Open Data (rhrread). Fetch every ~10 min via `eta_fetch_task` piggyback. Hide on failure/stale (30-min TTL)."
- §6 Architecture: add note that `eta_fetch_task` also owns weather fetch (every 20th cycle)

### 6.3 CLAUDE.md

- Add "Weather temperature" section: HKO rhrread endpoint, station config, piggyback cadence, TTL, spinlock pattern (like battery)
- Update "Permanent Never List" if needed (HKO response shape differs from KMB/Citybus — never assume interchangeability)

### 6.4 HANDOFF.md

Per handoff rule: update §1 (Last Completed Step) and §3 (Build Status) only. Do not regenerate.

---

## 7. Files to Create / Modify

| File | Action | Summary |
|------|--------|---------|
| `main/http_util.c` | **Create** | Extract `http_get_body` + `http_event_handler` from `eta_fetcher.c` |
| `main/http_util.h` | **Create** | Public API: `char *http_get_body(const char *url, const char *log_tag)` |
| `main/weather_hko.c` | **Create** | HKO fetch, JSON parse, spinlock-protected storage, `weather_snapshot`/`weather_get_temp_str` |
| `main/weather_hko.h` | **Create** | Public API: `weather_init`, `weather_fetch_once`, `weather_snapshot`, `weather_get_temp_str`, `weather_temp_t` struct |
| `main/eta_fetcher.c` | **Modify** | Remove `http_get_body`/`http_event_handler`/`body_capture_t`, include `http_util.h`, update call sites |
| `main/display.c` | **Modify** | `render_header` + `render_dashboard` signature changes; add temperature rendering block |
| `main/display.h` | **Modify** | Updated function signatures |
| `main/main.c` | **Modify** | `weather_init()` in `app_main`, weather fetch in `eta_fetch_task` (every 20th cycle), `weather_get_temp_str()` in `display_task` |
| `main/route_config.c` | **Modify** | Parse `"weather"."station"`, add `route_config_get_weather_station()` |
| `main/route_config.h` | **Modify** | Declare `route_config_get_weather_station()` |
| `main/CMakeLists.txt` | **Modify** | Add `http_util.c` and `weather_hko.c` to SRCS |
| `spiffs_data/routes.json` | **Modify** | Add `"weather": { "station": "Hong Kong Observatory" }` |
| `design.md` | **Done** | Temperature element added to header spec |
| `PRD.md` | **Modify** | FR #13, TBC #3 → Implemented, §6 architecture note |
| `CLAUDE.md` | **Modify** | Weather section, Never List if needed |
| `HANDOFF.md` | **Modify** | §1 + §3 only |

---

## 8. Verification Checklist

1. **Build**: `idf.py build` succeeds, binary fits 4 MB factory partition
2. **Happy path**: After ~10 min, header shows `NN°C` at 22 px bold, 16 px right of title, same baseline. Time and title unaffected.
3. **Degree symbol**: `"28°C"` renders `°` correctly. If not, fallback to `"28C"` (one-line change in `weather_get_temp_str`).
4. **Layout**: No overlap between temperature and time across typical ranges (`-5°C` to `40°C`). Temperature omitted if overlap guard triggers.
5. **Failure — Wi-Fi down**: ETA fetch fails, but weather counter still advances. After 30 min of no weather data, temperature disappears. Header shows title + time only.
6. **Failure — HKO API error**: HTTP non-200 or JSON parse failure → last-known temperature preserved until 30-min TTL, then hidden. No crash, no garbage.
7. **Failure — station not found**: Warning logged, temperature not updated. Last-known preserved until TTL.
8. **Boot**: First weather fetch runs on the first `eta_fetch_task` cycle (~30 s after boot). Temperature appears on the next render cycle after a successful fetch. If the first fetch fails, temperature remains hidden until a subsequent fetch succeeds.
9. **Logging**: Info log on successful update (station, value, epoch). Warnings on fetch failure, parse failure, missing station.
10. **No regression**: ETA fetch, display render, battery sampling all unaffected. No new task created. Wi-Fi PS toggling unchanged.
