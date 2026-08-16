# Plan: Fetch-all pages + out-of-service adaptive fetch interval

Status: **Implemented 2026-08-16** — build PASS (`0x4d1480`, ~5.05 MB, 40% app
partition free). Flash + on-device verification pending (user will flash).
All semantic decisions below were confirmed by the user via Q&A (2026-08-15);
implementation notes in §5 (files) and §6 (verification).

**Rev 2 (2026-08-16)** — build PASS (`0x4d13b0`): D9 reverted (page-toggle
fetch wake removed — see §3.1/D9), KMB "all entries skipped" warning now
carries the first skip reason (`missing eta` / `direction filter` /
`parse failure`), and the OOS window check + boundary cap use the clock
re-read after the fetch cycle (§4.2).

## 1. Goal

Two enhancements to the ETA dashboard:

1. **Fetch all routes, all pages** — `eta_fetch_task` currently fetches only
   the active page's routes (3 of 6), so toggling the page shows stale/blank
   data until the next fetch. Change it to fetch **every route in
   `routes.json`** every cycle, so any page toggle shows fresh ETAs
   immediately.
2. **Out-of-service adaptive fetch interval** — between 01:00 and 05:30,
   when all configured routes return no ETAs, the device is deemed out of
   service and the ETA fetch interval relaxes from 30 s to 300 s. Service
   resumes automatically at 05:30, or immediately if any route returns a
   non-empty ETA.

## 2. Confirmed decisions (Q&A record)

| # | Decision | Value |
|---|---|---|
| D1 | **OOS condition** | `time ∈ [oos_start, oos_end)` **AND** every configured route has `eta1 && eta2 && eta3 == -1` |
| D2 | **Daytime all-empty** | does NOT trigger OOS — stays in-service 30 s (a prolonged API/network outage must not slow recovery) |
| D3 | **Data wins at night** | any single non-empty ETA overrides the time window → in-service immediately |
| D4 | **Empty route definition** | all three ETA slots `-1` (a route with even one ETA counts as non-empty) |
| D5 | **Configurability** | values via `routes.json` keys (defaults reproduce current behaviour) |
| D6 | **Weather cadence** | time-based 600 s threshold (fetch when `now - last_weather_epoch >= 600`), state-independent, hardcoded `#define` (not configurable) |
| D7 | **OOS entry latency** | accepted — OOS is entered up to ~3.5 min after the last bus (180 s ETA expiry + next cycle); display already shows `--` during this lag |
| D8 | **Jitter vs. 05:30 cap** | jitter is ±10 % of the active interval, but the 05:30 boundary cap always wins (never overshoot past `oos_end`) |
| D9 | **Page-toggle notify** | **REMOVED** — with fetch-all, a toggle needs no immediate refetch (all pages fetched every cycle; the toggled page's data is ≤ one fetch interval old). Removes `s_eta_fetch_task_handle` + `xTaskNotifyGive`/`xTaskNotifyWait` (→ `vTaskDelay`), saving a full 6-route fetch per toggle. |

## 3. Enhancement 1 — fetch ALL routes, all pages

Current fetch loop in `main.c` `eta_fetch_task()`:

```c
int page = s_active_page;   /* snapshot once per cycle */
...
for (int i = 0; i < s_pages[page].count && i < ROUTES_PER_PAGE; i++) {
    int n = fetch_eta(&s_pages[page].routes[i], eta_buf, 3);
    ...
    s_route_buf[inactive][page][i].eta1 = ...;
    ...
    /* failure branch reads s_route_buf[active][page][i] */
}
```

### 3.1 Changes

- **Remove** the `page = s_active_page` snapshot and loop over all pages:
  outer `for (p = 0; p < s_page_count; p++)`, inner over
  `s_pages[p].routes[i]` (`i < s_pages[p].count && i < ROUTES_PER_PAGE`).
- Write into `s_route_buf[inactive][p][i]`; the failure-preservation branch
  reads `s_route_buf[active][p][i]` (per page, not hardwired to the active
  page).
- **Single atomic flip** at the end of the cycle still publishes all pages.
  The double-buffer invariant is preserved — `display_task` keeps reading
  `s_route_buf[buf_idx][page]` for the single visible page.
- **Remove** the page-toggle `xTaskNotifyGive` in `display_task` (D9): no immediate refetch needed — both pages are fetched every cycle, so a toggle always shows data ≤ one fetch interval old. `s_eta_fetch_task_handle` deleted and `xTaskNotifyWait` simplified to `vTaskDelay`, saving a full 6-route fetch (~20 s of radio time) per toggle.
- **Logging**: "Fetch cycle start (page %d)" becomes "Fetch cycle start
  (all %d routes / %d pages)".
- **Comments**: update the file-header comment and the double-buffer comment
  block that still say "Only the currently-active page is fetched".

## 4. Enhancement 2 — out-of-service adaptive interval

### 4.1 New `routes.json` keys (parsed in `route_config.c`)

| Key | Type | Default | Validation |
|---|---|---|---|
| `fetch_interval_seconds` | int | `30` | `5 <= v <= 3600`; else warn + default |
| `oos_fetch_interval_seconds` | int | `300` | `5 <= v <= 3600`; else warn + default |
| `oos_start` | "HH:MM" | `"01:00"` | parse → minutes-since-midnight; `start < end` required |
| `oos_end` | "HH:MM" | `"05:30"` | parse; same check |

New getters in `route_config.h`:
`route_config_get_fetch_interval()`, `route_config_get_oos_fetch_interval()`,
`route_config_get_oos_start_min()`, `route_config_get_oos_end_min()`.

These intervals are **not** render-aligned (unlike `refresh_seconds`), so
the divide-60 constraint does not apply.

### 4.2 State machine (inside `eta_fetch_task`)

Evaluated at the end of every fetch cycle against the **just-written inactive
buffer** (post-fetch, post-preservation), before arming the wait:

1. **Scan** all pages × configured routes only (`i < s_pages[p].count` —
   blank rows on partial pages are NOT routes and must not count):
   `all_empty = true` iff every route has `eta1 && eta2 && eta3 == -1`.
2. **Window check** (HKT, `localtime()`): `now_min = h*60 + m`;
   `in_window = (now_min >= oos_start_min) && (now_min < oos_end_min)`.
   The clock is **re-read after the fetch cycle**, so a cycle spanning
   05:30 restores in-service on this same cycle rather than one cycle later.
3. `oos = in_window && all_empty` (D1). Otherwise in-service.
4. **Log transitions only** (`ESP_LOGW`): "entering out-of-service mode
   (fetch interval 300 s)" / "leaving out-of-service mode (fetch interval
   30 s)". Track previous state in a local/static variable.
5. **Arm the wait** (see §4.3).

The OOS state stays **local to `eta_fetch_task`** — no display/footer change:
ETAs render `--` naturally, and `render_dashboard()` is untouched.

### 4.3 Delay arming (replaces the hardcoded `30000 + jitter`)

```c
int interval_ms = oos ? oos_interval_ms : fetch_interval_ms;
int half = interval_ms / 10;                                  /* ±10 % */
int jitter = (int)(esp_random() % (2 * half + 1)) - half;
int delay_ms = interval_ms + jitter;

if (oos) {
    /* Cap at the oos_end boundary so 05:30 always triggers a wake —
     * a 300 s wait armed at 04:50 must NOT run to 09:50 (D8). */
    int ms_to_end = oos_end_total_ms - now_total_ms;
    if (ms_to_end < delay_ms) delay_ms = ms_to_end;
    if (delay_ms <= 0) delay_ms = 10;   /* just past boundary: re-eval now */
}
vTaskDelay(pdMS_TO_TICKS(delay_ms));   /* plain delay — page switches never wake the fetch task (D9) */
```

`now_total_ms` / `oos_end_total_ms` are milliseconds-since-midnight
(`oos_end_min * 60000`). On the boundary wake the window check fails
(`now_min >= oos_end_min`) → in-service → 30 s cadence resumes immediately.

### 4.4 Interactions & consequences

- **Weather piggyback** — replace the cycle counter
  (`static int s_weather_cycle = 20`; `++ >= 20`) with a **time-based 600 s
  threshold** (D6):
  ```c
  static time_t s_last_weather_epoch = 0;   /* 0 → first cycle fetches */
  time(&now);
  if (now - s_last_weather_epoch >= WEATHER_FETCH_INTERVAL_S) {
      s_last_weather_epoch = now;           /* set regardless of success (parity) */
      weather_fetch_once();
  }
  ```
  Result at defaults: IS → every ~20 cycles (~600 s); OOS → every ~2 cycles
  (~600 s). Weather stays inside its 30-min TTL all night. The request rides
  an already-awake radio window (the OOS cycle wakes the radio for the ETA
  fetches anyway), so the 300 s power-saving goal is not undermined.
- **Battery sample** — `battery_sample_if_due()` stays every 2nd call; at
  night that becomes ~600 s. Cached value + hysteresis make this harmless.
- **OOS entry latency** (D7) — "all empty" materialises only after the last
  ETA timestamp is > 180 s in the past (existing expiry rule) plus the next
  fetch cycle (≤ 30 s). Display already shows `--` throughout; accepted.
- **API/network outage at night** — last-known-good ETAs (< 180 s) count as
  non-empty → stays in-service; only after expiry does all-empty → OOS.
  Matches the buffer-based `-1` definition (D4).
- **Clock-trust gate** — while untrusted, `eta_fetch_task` polls 500 ms and
  never fetches, so the state machine does not run. No interaction needed.
- **Display/render** — unchanged; footer `Updated HH:MM:SS` simply updates
  less often at night.

## 5. Files touched

| File | Change |
|---|---|
| `main/route_config.c` / `main/route_config.h` | Parse + validate 4 new keys; 4 new getters |
| `main/main.c` | Fetch-all loop (§3.1); OOS evaluation + configurable/capped delay (§4.2–4.3); time-based weather (§4.4); transition logging; comment updates |
| `spiffs_data/routes.json` | Add the 4 new keys explicitly (documented defaults make them optional; SPIFFS must be reflashed to take effect) |
| Docs | After implementation: `PRD.md` (new FR + resolved decisions), `CLAUDE.md` §Display Refresh (replace hardcoded 30 s text), `HANDOFF.md` (per workflow) |

## 6. Verification

1. Build clean, no new warnings (build PASS: `0x4d1480` / ~5.05 MB, 40% app partition free).
2. Flash; serial log confirms every cycle fetches all 6 routes ("Fetch cycle
   start (all 6 routes / 2 pages)").
3. Daytime: all routes `--` (e.g. temporary API outage) keeps 30 s interval.
4. Night: temporarily set `oos_start`/`oos_end` to bracket the current time
   (or wait) → transition log fires, interval becomes 300 s, weather still
   updates ~every 600 s.
5. Wake at `oos_end` → transition log fires, interval returns to 30 s.
6. Config validation: malformed `oos_start`/`oos_end` or out-of-range
   intervals → warning + defaults.
7. Page-toggle mid-OOS: page switches instantly, data for both pages intact.

## 7. Open items

- None blocking. D6/D7/D8/D9 are recommended defaults confirmed with the
  user during review; any change is a one-line deviation in §2.
