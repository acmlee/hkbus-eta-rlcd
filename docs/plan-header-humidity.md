# Plan: Relative humidity in header band

> Add current relative humidity (e.g. `70%`) beside the temperature in the
> header band, same font and same size as the temperature
> (`u8g2_font_profont22_mf`, 22 px bold). Data comes from the same HKO
> `rhrread` fetch that already supplies temperature — no new API call.

---

## 1. Confirmed Decisions

From user Q&A (2026-08-09):

| # | Decision |
|---|---|
| D1 | **Overlap drop order**: if temp+humidity+clock would collide, drop humidity first (keep temperature); if temperature alone still collides, omit the whole weather block (current behaviour). |
| D2 | **Missing humidity**: if the configured station has a temperature entry but no humidity entry in the response, show temperature alone (humidity simply absent). |
| D3 | **Format/spacing**: `NN%` (e.g. `70%`), drawn 12 px after the temperature, same font (`profont22_mf`) and baseline (y=24). |
| A1 | Humidity hides (no last-known-good retention) when the last successful fetch found no humidity entry for the station. |
| A2 | No new `routes.json` key — `weather.station` governs both temperature and humidity. |
| A3 | Struct renamed `weather_temp_t` → `weather_t` (contained rename: header def + 3 uses in `weather_hko.c` + 1 CLAUDE.md doc ref). |
| A4 | One shared 30-min TTL for the pair — both fields are written by the same fetch with the same `last_updated_epoch`, so they cannot diverge on staleness. |

---

## 2. Current State Analysis

- [weather_hko.c](file:///Users/alanlee/Documents/trae_projects/hkbus-eta-rlcd/main/weather_hko.c): `weather_fetch_once()` GETs HKO `rhrread` (`weather.php?dataType=rhrread&lang=en`), parses `temperature.data[]` for the configured station, and stores `temp_c` + `last_updated_epoch` + `valid` in `s_weather` under a spinlock. `weather_get_temp_str()` renders `NN°C`.
- The same response already contains `humidity.data[]` (`{unit:"percent", value:<int>, place:"<station>"}`) — verified live. `humidity.data` contains fewer stations than `temperature.data` (sample: only "Hong Kong Observatory").
- [display.c](file:///Users/alanlee/Documents/trae_projects/hkbus-eta-rlcd/main/display.c#L84-L126) `render_header()`: title at x=14; temperature at `x_temp = 14 + w_title + 16`, y=24, `profont22_mf`; time right-anchored at `DISP_WIDTH - 14`. Overlap guard: `x_temp + w_temp + 8 <= x_time_left` else omit.
- [main.c](file:///Users/alanlee/Documents/trae_projects/hkbus-eta-rlcd/main/main.c#L475-L480) `display_task`: builds `temp_str` via `weather_get_temp_str()`, passes `temp_ptr` into `render_dashboard()`.
- **Two call sites** of `render_dashboard()`: [display.c#L378](file:///Users/alanlee/Documents/trae_projects/hkbus-eta-rlcd/main/display.c#L378) (from `render_dashboard`) and [display.c#L420](file:///Users/alanlee/Documents/trae_projects/hkbus-eta-rlcd/main/display.c#L420) (DISPLAY_TEST self-test).
- **Overlap headroom**: clock width is fixed ("HH:MM" at logisoso28 ≈ 85 px → `x_time_left` ≈ 301); weather block ≈ 116 px ending ≈ 208 px. The drop-order guard is defensive — not triggerable with realistic data.

---

## 3. Proposed Changes

### 3.1 `main/weather_hko.h`
- Rename `weather_temp_t` → `weather_t`.
- Extend struct: add `int humidity_pct` and `bool humidity_valid`. Keep `temp_c`, `last_updated_epoch`, `valid` unchanged. Update doc comments.
- Add:
  ```c
  /**
   * @brief Convenience: fill a pre-formatted "NN%" string.
   * @param buf  Output buffer (recommended size 8 bytes minimum)
   * @param len  Buffer size
   * @return false if unavailable/stale, true if buffer was filled
   */
  bool weather_get_humidity_str(char *buf, size_t len);
  ```

### 3.2 `main/weather_hko.c`
- Rename `s_weather` type and `weather_snapshot()`/snapshot param to `weather_t`.
- `weather_fetch_once()`: after the temperature block, parse top-level `humidity` → `data` (array), match `place` against `s_station` (case-insensitive), require `unit == "percent"`.
  - Found: write `humidity_pct` (clamped 0–100, round-nearest) + `humidity_valid = true` in the same spinlock critical section as temperature (single critical section, single `last_updated_epoch` write).
  - Not found for the station: set `humidity_valid = false` (per A1 — hide, no last-known-good). This write happens only inside the already-successful temperature path (station matched for temperature).
- Update the success log line to include humidity.
- Add `weather_get_humidity_str()`: `weather_snapshot()` (TTL check shared per A4), then `if (!snap.humidity_valid) return false;` else `snprintf(buf, len, "%d%%", snap.humidity_pct);`.
- No new network call, no new task, no cadence change.

### 3.3 `main/display.h`
- Update signatures:
  ```c
  void render_header(const char *time_str, const char *temp_str, const char *hum_str);
  void render_dashboard(const char *time_str, const char *temp_str, const char *hum_str, ...);
  ```

### 3.4 `main/display.c`
- `render_header(time_str, temp_str, hum_str)`:
  - After drawing temperature, if `hum_str != NULL`: measure with `u8g2_GetUTF8Width(u, hum_str)` in `profont22_mf`, draw at `x_hum = x_temp + w_temp + 12`, y=24, `u8g2_SetDrawColor(u, 0)` (white-on-black).
  - Drop-order guard (D1):
    1. If both present: fit-check `x_temp + w_temp + 12 + w_hum + 8 <= x_time_left` → draw both.
    2. Else if `x_temp + w_temp + 8 <= x_time_left` → draw temperature only.
    3. Else → omit the whole weather block (current behaviour).
    4. If `hum_str == NULL` → behaviour identical to today (temperature-only fit-check).
- `render_dashboard(...)`: accept `hum_str`, pass through to `render_header`.
- Update **both** call sites:
  - `render_dashboard()` → `render_header(time_str, temp_str, hum_str)`.
  - DISPLAY_TEST: `render_dashboard("14:32", "28°C", "70%", "Updated 14:32:00", 255, NULL, test_routes, 3);` (visual test hook).

### 3.5 `main/main.c`
- In `display_task` section 4b: add
  ```c
  char hum_str[8] = {0};
  const char *hum_ptr = NULL;
  if (weather_get_humidity_str(hum_str, sizeof(hum_str))) {
      hum_ptr = hum_str;
  }
  ```
- Pass `hum_ptr` into `render_dashboard()`.

### 3.6 `design.md`
- **§2 typography table**: add row `| Header humidity (NN%) | 22 px | Bold | u8g2_font_profont22_mf |`.
- **§3 structural rule 1**: extend the three-element header list with humidity ("Temperature and humidity must never overlap the time element"); update the ASCII diagram `28°C` → `28°C 70%`.
- **§7 checklist**: extend the header item to include humidity; add item verifying humidity hides independently (station without humidity entry).
- **§8** retitle to `External Data: Weather (Temperature & Humidity)`; document the humidity element: source `humidity.data[]` of same rhrread response, same station, format `NN%`, 12 px gap, same TTL, hidden independently when unavailable/stale, drop order D1.

### 3.7 `CLAUDE.md`
- Section "Weather Temperature" → "Weather Temperature & Humidity". Update:
  - Data model bullet: struct renamed `weather_t`, includes `humidity_pct`/`humidity_valid`.
  - New bullets: format `NN%` (`profont22_mf`, 12 px gap, y=24 baseline); shared 30-min TTL; humidity hidden independently when station lacks a humidity entry; drop order on overlap; reader convenience `weather_get_humidity_str()`.

### 3.8 `PRD.md`
- **FR #13** (line 43): extend to "Temperature and humidity display" — humidity from the same HKO `rhrread` fetch and station, same fetch cadence/TTL, format `NN%`, hidden independently when unavailable/stale.
- **TBC #3** (line 203): update note to mention humidity is implemented (same fetch, `humidity.data[]`), format `70%`, drop order, independent hide.

### 3.9 `HANDOFF.md`
- Update §1 (Last Completed Step), §2 (Files Touched This Session), §3 (Build Status) after the build per session workflow.

---

## 4. Verification

1. `idf.py build` — expect PASS, no new warnings; binary size delta ~0 (same font, no new data tables).
2. `idf.py -DCMAKE_C_FLAGS="-DDISPLAY_TEST=1" build` + flash — visually confirm `28°C 70%` renders in the header at the correct size/position/baseline.
3. Real-device flash — confirm `NN°C NN%` in the header with default station ("Hong Kong Observatory").
4. Set `weather.station` to `"Sha Tin"` in [routes.json](file:///Users/alanlee/Documents/trae_projects/hkbus-eta-rlcd/spiffs_data/routes.json#L68) — confirm temperature alone renders (station has no humidity entry).
5. Overlap drop order: verified by reasoning (unreachable at realistic widths); optionally force via a temporary `DISP_WIDTH` shrink during DISPLAY_TEST only — not committed.

---

## 5. Out of Scope

- No new API/station/key in `routes.json`.
- No humidity in footer, no labels ("RH"), no separators.
- No changes to fetch cadence, TTL policy for temperature, or task architecture.
- Historical plan docs (`docs/plan-second-page.md`, `docs/plan-display-sleep-button-wake.md`) are not touched.
