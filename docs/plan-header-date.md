# Plan: Replace header label "HK Bus ETA" with current date

> Replace the static left-aligned header label `"HK Bus ETA"` with the current
> date in the format `DD MMM (DDD)` (e.g. `9 Aug (Sun)`). When the day is a
> single digit, render a space in the tens slot (` 9 Aug (Sun)`) so the ones
> digit sits at a stable x-position as the date increments.

---

## 1. Confirmed Decisions

From user Q&A (2026-08-09):

| # | Decision |
|---|---|
| D1 | **Font**: keep `u8g2_font_helvR10_tr` (10 px regular) — same as today's label. Consistent with design.md §2 header-label spec. |
| D2 | **No-shift rule = align ones digit only**: ` 9 Aug (Sun)` (space in tens slot). The label's left edge is fixed at x=14; its width still varies day-to-day with month/weekday names, so the weather block shifts a few px — accepted. Existing drop-order guard (humidity → temperature → whole weather block, time always wins) protects the time. |
| D3 | **Language**: English month/weekday abbreviations (`Aug`, `Sun`). Newlib C locale gives English `%b`/`%a` — no locale setup needed. |
| D4 | **Pre-sync placeholder**: before the clock is valid, show `-- --- (---)` instead of a 1970-era date. Reuse the existing sync threshold `now >= 1700000000` (same as `ntp_wait_for_sync()`). |
| D5 | **No monospace font** — D2 accepted as-is; a fixed-total-width label (which would require monospace) is explicitly out of scope. |

---

## 2. Current State Analysis

- [display.c](file:///Users/alanlee/Documents/trae_projects/hkbus-eta-rlcd/main/display.c#L84-L136) `render_header()`: draws the literal `"HK Bus ETA"` at (14, 24) with `u8g2_font_helvR10_tr`; weather anchor `x_temp = 14 + w_title + 16` where `w_title` is the measured width of the literal `"HK Bus ETA"` (line 103); time right-anchored at `DISP_WIDTH - 14` (never moves).
- [main.c](file:///Users/alanlee/Documents/trae_projects/hkbus-eta-rlcd/main/main.c#L420-L448) `display_task()`: builds `time_str` from `localtime()` at the top of the loop and again after the 06:00 resync. No date string exists today.
- Sync threshold literal `1700000000` used in `ntp_wait_for_sync()` at [main.c:200](file:///Users/alanlee/Documents/trae_projects/hkbus-eta-rlcd/main/main.c#L200) and [main.c:205](file:///Users/alanlee/Documents/trae_projects/hkbus-eta-rlcd/main/main.c#L205).
- **Call sites** of `render_dashboard()`: `display_task()` in main.c and DISPLAY_TEST in [display.c:431](file:///Users/alanlee/Documents/trae_projects/hkbus-eta-rlcd/main/display.c#L431).
- `strftime("%d")` zero-pads; `%e` (space-padded day) support in newlib is not assumed — build the day field manually.
- Longest strings: `"30 Sep (Wed)"` = 12 chars; placeholder `"-- --- (---)"` = 13 chars. A 24-byte buffer is ample.
- Midnight rollover needs no special handling — the header re-renders every refresh interval and re-reads `localtime()`.

---

## 3. Proposed Changes

### 3.1 `docs/plan-header-date.md`
- This file (the approved plan).

### 3.2 `main/display.h`
- Add `const char *date_str` as the **first** parameter of `render_header()` and `render_dashboard()`.

### 3.3 `main/display.c`
- `render_header(const char *date_str, ...)`:
  - Draw `date_str` instead of the literal `"HK Bus ETA"` at (14, 24).
  - Measure `w_title` against `date_str` (replaces the literal at line 103) — keeps weather anchored to the label's right edge per D2.
  - Update comments (`"HK Bus ETA"` → date label).
- `render_dashboard(...)`: accept `date_str`, pass through to `render_header`.
- DISPLAY_TEST call site: pass a sample date, e.g. `" 9 Aug (Sun)"`.

### 3.4 `main/main.c`
- Extract the sync threshold to a file-scope constant, e.g. `#define EPOCH_SYNC_THRESHOLD 1700000000UL`, and use it in `ntp_wait_for_sync()` (lines 200/205) and the new date build.
- In `display_task()`: add `char date_str[24];`. Build it wherever `time_str` is built (top of loop and the 06:00-resync re-read):
  - If `now < EPOCH_SYNC_THRESHOLD` → copy `"-- --- (---)"`.
  - Else:
    - day: `tm_mday < 10 ? " %d" : "%d"` → `" 9"` / `"30"`.
    - month + weekday: `strftime("%b (%a)", ...)` → `"Aug (Sun)"`.
    - combine → `" 9 Aug (Sun)"`.
- Pass `date_str` into `render_dashboard()`.

### 3.5 `design.md`
- §2 typography table row `Header label ("HK Bus ETA")` → `Header date (DD MMM (DDD))`, same font `u8g2_font_helvR10_tr`.
- §2 weight bullet: update the header-label rationale (now a date, not a static title; still regular weight).
- §3 structural rule 1: `"HK Bus ETA"` → date; document the space-padding rule for single-digit days and the pre-sync placeholder; update the ASCII diagram (`HK Bus ETA` → ` 9 Aug (Sun)`).
- §7 checklist: update the header item to describe the date and its rules.

### 3.6 `CLAUDE.md`
- Weather section: update "gap from title" wording → "gap from date label".
- Add a concise "Header Date" section: format `DD MMM (DDD)`, single-digit day keeps a space in the tens slot (D2), English `%b`/`%a`, font `u8g2_font_helvR10_tr`, pre-sync placeholder `-- --- (---)` (threshold `now >= 1700000000`), midnight rollover automatic, weather anchored to the date label's right edge.

### 3.7 `PRD.md`
- No change required — the PRD never specifies the `"HK Bus ETA"` label (FR 3/FR 7 only mandate `HH:MM` in the header).

### 3.8 `mockup.html` / `gallery.html`
- Out of scope — they were not updated for the humidity feature either; stale relative to the current design.

### 3.9 `HANDOFF.md`
- Update §1 (Last Completed Step), §2 (Files Touched This Session), §3 (Build Status) after the build per session workflow.

---

## 4. Verification

1. `idf.py build` — expect PASS, no new warnings; binary size delta negligible (no new data tables).
2. `idf.py -DCMAKE_C_FLAGS="-DDISPLAY_TEST=1" build` — compile-verifies the DISPLAY_TEST call site.
3. On-device (user flashes): date in English HKT for the current day (e.g. ` 9 Aug (Sun)` for 2026-08-09), single-digit space visible, placeholder `-- --- (---)` during the first seconds after boot, weather/time positions correct, no overlap regression.

---

## 5. Out of Scope

- No monospace font / fixed-total-width label (D5).
- No date in the footer, no page indicator changes, no routes.json changes.
- No change to weather drop-order logic, time anchoring, or task architecture.
- Historical plan docs not touched.
