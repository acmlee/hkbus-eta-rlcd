# Plan: Second Page of Bus ETA Display (Page Toggle via KEY Button)

**Status**: **Implemented 2026-07-26** — build PASS, on-device verified.
**Created**: 2026-07-26
**Tracking**: PRD.md §10 Open/TBC Decisions #6

> **Superseded in part (2026-08-16)** — The fetch strategy below ("fetch only the
> visible page" + page-toggle `xTaskNotifyGive` early-wake) was **replaced** by
> fetch-all-pages: `eta_fetch_task` now fetches **every route every cycle** and the
> page-toggle no longer wakes the fetch task (decision D9 of `docs/plan-fetch-all-oos.md`).
> Sections marked **[SUPERSEDED]** below describe the original design and are retained
> as historical record only — the current behaviour is in CLAUDE.md §Multi-Page Display.

---

## Background

The dashboard currently shows exactly 3 routes on a single page. This plan adds a
**second page** with up to 3 more routes, toggled manually via the on-board KEY button
(GPIO18). **[SUPERSEDED]** Only the visible page is fetched from the ETA APIs
(power-efficient); a button press switches pages and triggers an immediate fetch for the
newly-visible page. *(Current: all pages are fetched every cycle; toggling never triggers
a fetch — see `docs/plan-fetch-all-oos.md`.)*

---

## Decisions (confirmed with user)

| Decision | Choice | Rationale |
|---|---|---|
| JSON structure | `pages` array of `{routes: [...]}` | Most explicit, scales beyond 2 pages, self-documenting. |
| Fetch strategy | **[SUPERSEDED]** Fetch only the visible page | Saves power when page 2 is unused. *(Current: fetch-all — D9 of plan-fetch-all-oos.md.)* |
| Toggle mode | Manual only (no auto-rotate) | Matches "User may press KEY to toggle" intent. Lowest power. |
| GPIO18 ownership | Page toggle wins short-press | The pending sleep plan (`docs/plan-display-sleep-button-wake.md`) must be reworked later (long-press discriminator or different button). |
| Switch latency | **[SUPERSEDED]** Immediate fetch on switch (`xTaskNotifyGive`) | *(Removed — fetch-all means the toggled page's data is ≤ one fetch interval old; the `xTaskNotifyGive`/`xTaskNotifyWait` pair was deleted in Rev 2 of plan-fetch-all-oos.md.)* |
| Page 2 presence | Optional, graceful degradation | If absent, device runs as 1-page system; button no-op; footer hides indicator. |
| Partial pages | Blank rows for missing routes | Cleanest visually; no placeholder "--" rows. |
| Page 2 content | Placeholder (duplicated from page 1) | User will fill in real routes later. |

---

## 1. routes.json — New Structure

### 1.1 New schema

Top-level `"pages"` array, each page object contains a `"routes"` array. Legacy
top-level `"routes"` is accepted as a single-page fallback (backward compatibility).

```json
{
  "pages": [
    { "routes": [ /* up to 3 route objects */ ] },
    { "routes": [ /* up to 3 route objects */ ] }
  ],
  "refresh_seconds": 10,
  "weather": { "station": "..." }
}
```

Each route object shape is unchanged from today:
```json
{
  "operator": "KMB" | "CTB",
  "route": "30X",
  "stop_id": "92A8281D80524F78",
  "stop_en": "Tai Ho Road Tsuen Wan",
  "stop_zh": "荃灣大河道",
  "dest_en": "Whampoa Garden",
  "dest_zh": "黃埔花園"
}
```

### 1.2 Proposed routes.json (page 2 is actual routes)

> **NOTE**: Page 2 routes are actual routes to be shown on the dashboard.

```json
{
  "pages": [
    {
      "routes": [
        { "operator": "KMB", "route": "30X",  "stop_id": "92A8281D80524F78",
          "stop_en": "Tai Ho Road Tsuen Wan", "stop_zh": "荃灣大河道",
          "dest_en": "Whampoa Garden",         "dest_zh": "黃埔花園" },
        { "operator": "KMB", "route": "238X", "stop_id": "88E1C9BB0B80711F",
          "stop_en": "Riviera Gardens Bus Terminus", "stop_zh": "海濱花園總站",
          "dest_en": "China Ferry Terminal", "dest_zh": "中港碼頭" },
        { "operator": "CTB", "route": "930X", "stop_id": "003449",
          "stop_en": "Luen Yan Street",       "stop_zh": "聯仁街",
          "dest_en": "Causeway Bay",          "dest_zh": "銅鑼灣" }
      ]
    },
    {
      "routes": [
        {
          "operator": "KMB",
          "route": "49X",
          "stop_id": "A7E87E8D797D1A52",
          "stop_en": "WANG LUNG STREET TSUEN WAN (TW272)",
          "stop_zh": "荃灣橫龍街",
          "dest_en": "KWONG YUEN",
          "dest_zh": "廣源"
        },
        {
          "operator": "KMB",
          "route": "A31",
          "stop_id": "35DB8602F51CF60F",
          "stop_en": "MA TAU PA ROAD, TSUEN WAN (TW115)",
          "stop_zh": "荃灣馬頭壩道",
          "dest_en": "AIRPORT (GROUND TRANSPORTATION CENTRE)",
          "dest_zh": "機場"
        },
        {
          "operator": "KMB",
          "route": "33B",
          "stop_id": "3357B55DE2539CA3",
          "stop_en": "LUEN YAN STREET TSUEN WAN (TW285)",
          "stop_zh": "荃灣聯仁街",
          "dest_en": "YAU TONG",
          "dest_zh": "油塘"
        }
      ]
    }
  ],
  "refresh_seconds": 10,
  "weather": {
    "station": "Hong Kong Observatory"
  }
}
```

### 1.3 Backward-compatibility rule in `route_config.c`

- If top-level `"pages"` array exists → load each page's `"routes"` array.
- Else if top-level `"routes"` array exists (legacy) → synthesize a single page (page 0).
- Else → error, no routes loaded.
- `MAX_PAGES = 2`. Extra pages beyond 2 are ignored with a warning.
- Per-page route count: capped at 3 (matches `ZONE_ROW_COUNT`). Extra entries logged and skipped.

---

## 2. Module: `route_config` (config layer)

### 2.1 `main/route_config.h` changes

```c
#define MAX_PAGES        2
#define ROUTES_PER_PAGE  3

typedef struct {
    route_config_t routes[ROUTES_PER_PAGE];
    int count;                          /* 0..3 */
} page_config_t;

/**
 * @brief Load pages from SPIFFS (routes.json).
 *
 * Supports both new "pages" array format and legacy top-level "routes" array
 * (treated as a single page). Fills pages[] up to max_pages entries.
 *
 * @param pages     Output array of page configs
 * @param max_pages Capacity of the array (typically MAX_PAGES)
 * @return Number of pages loaded (1 or 2), or 0 on error.
 */
int route_config_load_pages(page_config_t pages[], int max_pages);

/* Existing accessors unchanged */
int       route_config_get_refresh_interval(void);
const char *route_config_get_weather_station(void);
```

The old `route_config_load()` prototype is removed (single caller in `main.c` will be migrated).

### 2.2 `main/route_config.c` changes

- Refactor the existing per-route field-extraction logic (operator/route/stop_id/dest_zh/dest_en/stop_zh) into a static helper:
  ```c
  static bool parse_one_route(cJSON *item, route_config_t *out, int index);
  ```
  Returns `true` on success, `false` on skip (unknown operator, etc.). Logs per-route warnings as today.
- New top-level function `route_config_load_pages()`:
  ```c
  int route_config_load_pages(page_config_t pages[], int max_pages)
  {
      /* SPIFFS mount, read file, cJSON_Parse — same as today */
      ...

      /* refresh_seconds + weather parsing — unchanged */
      ...

      cJSON *pages_arr = cJSON_GetObjectItem(root, "pages");
      int page_count = 0;

      if (cJSON_IsArray(pages_arr)) {
          /* New format: pages array */
          int n = cJSON_GetArraySize(pages_arr);
          if (n > max_pages) {
              ESP_LOGW(TAG, "pages array has %d entries, capping to %d", n, max_pages);
              n = max_pages;
          }
          for (int p = 0; p < n; p++) {
              cJSON *page_obj = cJSON_GetArrayItem(pages_arr, p);
              cJSON *routes_arr = cJSON_GetObjectItem(page_obj, "routes");
              if (!cJSON_IsArray(routes_arr)) {
                  ESP_LOGW(TAG, "page %d: 'routes' not array, treating as empty", p);
                  pages[p].count = 0;
                  continue;
              }
              int rn = cJSON_GetArraySize(routes_arr);
              if (rn > ROUTES_PER_PAGE) {
                  ESP_LOGW(TAG, "page %d: %d routes, capping to %d", p, rn, ROUTES_PER_PAGE);
                  rn = ROUTES_PER_PAGE;
              }
              int filled = 0;
              for (int i = 0; i < rn; i++) {
                  cJSON *item = cJSON_GetArrayItem(routes_arr, i);
                  if (parse_one_route(item, &pages[p].routes[filled], i)) {
                      filled++;
                  }
              }
              pages[p].count = filled;
              page_count++;
          }
      } else {
          /* Legacy: top-level "routes" array → single page */
          cJSON *routes_arr = cJSON_GetObjectItem(root, "routes");
          if (cJSON_IsArray(routes_arr)) {
              int rn = cJSON_GetArraySize(routes_arr);
              if (rn > ROUTES_PER_PAGE) rn = ROUTES_PER_PAGE;
              int filled = 0;
              for (int i = 0; i < rn; i++) {
                  cJSON *item = cJSON_GetArrayItem(routes_arr, i);
                  if (parse_one_route(item, &pages[0].routes[filled], i)) {
                      filled++;
                  }
              }
              pages[0].count = filled;
              page_count = 1;
              ESP_LOGI(TAG, "Legacy routes.json format → single page");
          } else {
              ESP_LOGE(TAG, "Neither 'pages' nor 'routes' array found");
          }
      }

      cJSON_Delete(root);
      return page_count;
  }
  ```
- `refresh_interval` and `s_weather_station` static state + accessors: unchanged.

---

## 3. Module: `button` (new — reusable GPIO18 driver)

### 3.1 `main/button.h` (new)

```c
#pragma once
#include <stdint.h>

#define BUTTON_GPIO 18   /* KEY button on Waveshare ESP32-S3-RLCD-4.2 */

/**
 * @brief Configure GPIO18 (KEY button) as input with internal pull-up
 *        and falling-edge interrupt. Call once at boot.
 */
void button_init(void);

/**
 * @brief Atomically return the number of presses since the last call,
 *        then reset the counter to 0. Returns 0 if no presses.
 *
 * Multiple bounces within one poll interval collapse into a single
 * non-zero return at the consumer side.
 */
uint32_t button_consume_presses(void);

/**
 * @brief Non-destructive peek of current press count (for debug).
 */
uint32_t button_get_press_count(void);
```

### 3.2 `main/button.c` (new)

- `gpio_config_t`: mode input, pull-up enable, intr_type falling-edge.
- ISR: increment `volatile uint32_t s_press_count`. Use `__atomic_add_fetch` or rely on the
  fact that a 32-bit increment on ESP32-S3 is a single instruction (acceptable for a counter
  that is consumed periodically). Prefer `gpio_isr_handler_add` from `esp_driver_gpio`.
- `button_init()`:
  ```c
  gpio_install_isr_service(0);   /* no flags; safe to call if already installed */
  gpio_config_t io = {
      .pin_bit_mask = 1ULL << BUTTON_GPIO,
      .mode         = GPIO_MODE_INPUT,
      .pull_up_en   = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type    = GPIO_INTR_NEGEDGE,
  };
  gpio_config(&io);
  gpio_isr_handler_add(BUTTON_GPIO, button_isr, NULL);
  ```
- ISR handler:
  ```c
  static void IRAM_ATTR button_isr(void *arg)
  {
      (void)arg;
      __atomic_add_fetch(&s_press_count, 1, __ATOMIC_SEQ_CST);
  }
  ```
- `button_consume_presses()`:
  ```c
  uint32_t button_consume_presses(void)
  {
      return __atomic_exchange_n(&s_press_count, 0, __ATOMIC_SEQ_CST);
  }
  ```
- **Debouncing**: No software debounce. Contact bounce (~10 ms) produces multiple ISR
  fires within one `display_task` poll interval (default 10 s). All bounces collapse into
  one non-zero `button_consume_presses()` return, which the consumer treats as "at least
  one press" → one toggle. Exact count is not critical.

### 3.3 Coexistence with the pending sleep plan

Per the user's decision, **page-toggle owns GPIO18 short-press**. When
`docs/plan-display-sleep-button-wake.md` is eventually implemented, it must either:

- Adopt a long-press vs short-press discriminator in `button.c` (e.g. measure press
  duration in the ISR using `xTaskGetTickCountFromISR`), with short-press → page toggle,
  long-press → display sleep wake; or
- Be reassigned to a different physical button (none currently available on this board).

The `button.c` driver is intentionally minimal (press-count semantics) so it can be
extended later without breaking this feature. The pending sleep plan's `button.c` design
must be merged with this one rather than replaced.

---

## 4. Module: `display` (render layer)

### 4.1 `main/display.h` changes

```c
/* Updated signatures */
void render_footer(const char *updated_str, int battery_pct,
                   const char *page_indicator_str);   /* NEW param; NULL = hide */

void render_dashboard(const char *time_str, const char *temp_str,
                      const char *updated_str, int battery_pct,
                      const char *page_indicator_str,   /* NEW */
                      const route_data_t routes[3],
                      int route_count);                 /* NEW: 1..3 */
```

`route_count` tells `render_dashboard` how many rows to draw. Rows beyond `route_count`
are left **blank** (no route number, no destination, no ETA, no divider beyond those
between actual rows).

### 4.2 `main/display.c` changes

**`render_footer()`** — add `page_indicator_str` parameter:

```c
void render_footer(const char *updated_str, int battery_pct,
                   const char *page_indicator_str)
{
    u8g2_t *u = u8g2();

    u8g2_SetDrawColor(u, 1);
    u8g2_DrawBox(u, 0, ZONE_FOOTER_Y, DISP_WIDTH, ZONE_FOOTER_H);

    char pct_buf[16];
    if (battery_pct == 255) snprintf(pct_buf, sizeof(pct_buf), "Battery:  --%%");
    else                    snprintf(pct_buf, sizeof(pct_buf), "Battery: %3d%%", battery_pct);

    u8g2_SetFont(u, u8g2_font_profont12_mf);
    u8g2_SetDrawColor(u, 0);  /* white-on-black */

    /* Left: "Updated HH:MM:SS" */
    u8g2_DrawStr(u, 14, ZONE_FOOTER_Y + 14, updated_str);

    /* NEW: Page indicator 10 px after Updated text */
    if (page_indicator_str != NULL) {
        int w_updated = u8g2_GetStrWidth(u, updated_str);
        int x_page = 14 + w_updated + 10;
        u8g2_DrawStr(u, x_page, ZONE_FOOTER_Y + 14, page_indicator_str);
    }

    /* Right: "Battery: XX%" */
    int tw = u8g2_GetStrWidth(u, pct_buf);
    u8g2_DrawStr(u, DISP_WIDTH - 14 - tw, ZONE_FOOTER_Y + 14, pct_buf);

    u8g2_SetDrawColor(u, 1);
}
```

**Width budget**: "Updated HH:MM:SS" ≈ 110 px + 10 + "Page 1/2" ≈ 50 px = ~170 px from
left. "Battery: 100%" ≈ 90 px from right. Total ~260 px in a 400 px band → comfortable,
no overlap risk.

**`render_dashboard()`** — iterate only `route_count` rows:

```c
void render_dashboard(const char *time_str, const char *temp_str,
                      const char *updated_str, int battery_pct,
                      const char *page_indicator_str,
                      const route_data_t routes[3], int route_count)
{
    u8g2_t *u = u8g2();
    u8g2_ClearBuffer(u);

    render_header(time_str, temp_str);

    for (int i = 0; i < route_count && i < 3; i++) {
        if (i > 0) render_divider(row_y(i) - 1);
        render_route_row(i, routes[i].route_num, routes[i].dest_zh,
                         routes[i].stop_zh, routes[i].eta1,
                         routes[i].eta2, routes[i].eta3);
    }
    /* Rows i >= route_count are left blank — no drawing */

    render_footer(updated_str, battery_pct, page_indicator_str);

    /* Diagnostic logging unchanged */
    ...
    render_flush();
}
```

**`render_route_row()`** — unchanged.

### 4.3 Footer layout (visual)

```
┌──────────────────────────────────────────────────────────┐
│ Updated 14:32:00  Page 1/2                  Battery: 72% │
└──────────────────────────────────────────────────────────┘
                  ^^^^^^^^^^ 10 px after Updated text
```

When single-page mode (page 2 absent): `page_indicator_str = NULL` → footer reverts to
current two-element layout (Updated + Battery).

---

## 5. Module: `main.c` (orchestration)

### 5.1 New static state

```c
static page_config_t s_pages[MAX_PAGES];              /* loaded once at boot */
static int           s_page_count = 0;                /* 1 or 2 */

/* Double-buffer now per-page: [buf][page][route] */
static route_data_t  s_route_buf[2][MAX_PAGES][ROUTES_PER_PAGE];
static volatile int  s_active_buf_idx = 0;            /* 0 or 1 — atomic */
static volatile int  s_active_page    = 0;            /* 0 or 1 — atomic */
static TaskHandle_t  s_eta_fetch_task_handle = NULL;  /* for xTaskNotifyGive */
```

### 5.2 `app_main()` changes

- Replace `route_config_load(s_routes, MAX_ROUTES)` with
  `route_config_load_pages(s_pages, MAX_PAGES)`.
- Initialize **both pages × both buffers** with static route info and `-1` ETA sentinels:

  ```c
  for (int buf = 0; buf < 2; buf++) {
      for (int p = 0; p < s_page_count; p++) {
          for (int i = 0; i < ROUTES_PER_PAGE; i++) {
              if (i < s_pages[p].count) {
                  s_route_buf[buf][p][i].route_num = s_pages[p].routes[i].route;
                  s_route_buf[buf][p][i].dest_zh   = s_pages[p].routes[i].dest_zh;
                  s_route_buf[buf][p][i].stop_zh   = s_pages[p].routes[i].stop_zh;
              } else {
                  /* Blank row */
                  s_route_buf[buf][p][i].route_num = "";
                  s_route_buf[buf][p][i].dest_zh   = "";
                  s_route_buf[buf][p][i].stop_zh   = "";
              }
              s_route_buf[buf][p][i].eta1 = (time_t)-1;
              s_route_buf[buf][p][i].eta2 = (time_t)-1;
              s_route_buf[buf][p][i].eta3 = (time_t)-1;
          }
      }
  }
  s_active_buf_idx = 0;
  s_active_page    = 0;
  ```

- `button_init()` after `display_init()`.
- `xTaskCreate(eta_fetch_task, ...)` → save handle to `s_eta_fetch_task_handle`.
- `display_task` creation unchanged (priority, stack).

### 5.3 `eta_fetch_task()` changes

> **[SUPERSEDED]** — This snippet fetches only the visible page and uses
> `xTaskNotifyWait` for page-switch early-wake. Since Rev 2 of `docs/plan-fetch-all-oos.md`
> the loop fetches **all pages × routes** and the wait is a plain `vTaskDelay` (page
> switches never wake the fetch task). See plan-fetch-all-oos.md §3.1 and CLAUDE.md
> §Multi-Page Display for the current implementation.

Replace the fetch loop and the bottom delay:

```c
static void eta_fetch_task(void *arg)
{
    eta_entry_t eta_buf[3];
    static int s_weather_cycle = 20;
    (void)arg;

    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

    while (1) {
        ESP_LOGD(TAG, "Fetch cycle start (page %d)", s_active_page + 1);

        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

        int inactive = 1 - s_active_buf_idx;
        int page     = s_active_page;        /* snapshot once per cycle */

        /* Fetch only the visible page's routes */
        for (int i = 0; i < s_pages[page].count && i < ROUTES_PER_PAGE; i++) {
            int n = fetch_eta(&s_pages[page].routes[i], eta_buf, 3);
            if (n > 0) {
                s_route_buf[inactive][page][i].eta1 = (n >= 1) ? eta_buf[0].eta_epoch : (time_t)-1;
                s_route_buf[inactive][page][i].eta2 = (n >= 2) ? eta_buf[1].eta_epoch : (time_t)-1;
                s_route_buf[inactive][page][i].eta3 = (n >= 3) ? eta_buf[2].eta_epoch : (time_t)-1;
            } else {
                /* Preserve last-known-good from active buffer (same page),
                 * expire if > 180 s in the past — same logic as today. */
                int active = s_active_buf_idx;
                time_t now;
                time(&now);
                time_t e1 = s_route_buf[active][page][i].eta1;
                time_t e2 = s_route_buf[active][page][i].eta2;
                time_t e3 = s_route_buf[active][page][i].eta3;

                s_route_buf[inactive][page][i].eta1 =
                    (e1 != (time_t)-1 && difftime(now, e1) < 180.0) ? e1 : (time_t)-1;
                s_route_buf[inactive][page][i].eta2 =
                    (e2 != (time_t)-1 && difftime(now, e2) < 180.0) ? e2 : (time_t)-1;
                s_route_buf[inactive][page][i].eta3 =
                    (e3 != (time_t)-1 && difftime(now, e3) < 180.0) ? e3 : (time_t)-1;
            }

            /* Static fields are stable; refresh defensively in case
             * routes.json was changed (it isn't, but cheap). */
            s_route_buf[inactive][page][i].route_num = s_pages[page].routes[i].route;
            s_route_buf[inactive][page][i].dest_zh   = s_pages[page].routes[i].dest_zh;
            s_route_buf[inactive][page][i].stop_zh   = s_pages[page].routes[i].stop_zh;

            vTaskDelay(pdMS_TO_TICKS(100));   /* yield to WiFi driver */
        }

        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
        battery_sample_if_due();

        if (++s_weather_cycle >= 20) {
            s_weather_cycle = 0;
            weather_fetch_once();
        }

        s_active_buf_idx = inactive;   /* atomic publish */

        /* Wait ~30 s OR early-wake on page switch notification.
         * xTaskNotifyWait returns immediately if a notification is
         * already pending (latched), triggering a fresh fetch for
         * the newly-active page. */
        int jitter  = (int)(esp_random() % 6001) - 3000;
        int delay_ms = 30000 + jitter;
        xTaskNotifyWait(0, 0, NULL, pdMS_TO_TICKS(delay_ms));
    }
}
```

Key points:
- **Only the active page is fetched.** Inactive page's data stays stale (or at sentinel
  `-1` if never visited).
- `xTaskNotifyWait` replaces `vTaskDelay` — a notification from `display_task` (page
  switch) breaks the wait immediately, triggering a fresh fetch for the newly-active page.
- Notification is latched: if it arrives mid-fetch, the next `xTaskNotifyWait` returns
  immediately.

### 5.4 `display_task()` changes

> **[SUPERSEDED]** — Step 3 below sends `xTaskNotifyGive(s_eta_fetch_task_handle)` on page
> toggle. This was **removed** in Rev 2 of plan-fetch-all-oos.md (fetch-all makes the
> wake unnecessary; the handle is deleted and the toggle only flips `s_active_page`).
> Steps 4–10 remain as implemented.

```c
static void display_task(void *arg)
{
    char last_updated[32];
    char time_str[6];
    int last_resync_yday = -1;
    static int battery_pct = 255;
    (void)arg;

    while (1) {
        /* ---- 1. Read wall-clock time ---- */
        time_t now;
        time(&now);
        struct tm *ti = localtime(&now);

        snprintf(time_str, sizeof(time_str), "%02d:%02d",
                 ti->tm_hour, ti->tm_min);

        /* ---- 2. Daily NTP resync at 06:00 (unchanged) ---- */
        if (ti->tm_hour == 6 && ti->tm_yday != last_resync_yday) {
            ntp_resync();
            last_resync_yday = ti->tm_yday;
            time(&now);
            ti = localtime(&now);
            snprintf(time_str, sizeof(time_str), "%02d:%02d",
                     ti->tm_hour, ti->tm_min);
        }

        /* ---- 3. NEW: Consume button presses → toggle page ---- */
        uint32_t presses = button_consume_presses();
        if (presses > 0 && s_page_count > 1) {
            s_active_page = 1 - s_active_page;   /* atomic toggle */
            ESP_LOGI(TAG, "Page switch → page %d/%d",
                     s_active_page + 1, s_page_count);

            /* Wake eta_fetch_task immediately so the new page's ETAs
             * refresh without waiting up to ~30 s. */
            xTaskNotifyGive(s_eta_fetch_task_handle);
        }

        /* ---- 4. Snap active buffer + page (single atomic reads) ---- */
        int buf_idx = s_active_buf_idx;
        int page    = s_active_page;

        /* ---- 5. Build "Updated HH:MM:SS" (unchanged) ---- */
        time(&now);
        ti = localtime(&now);
        snprintf(last_updated, sizeof(last_updated),
                 "Updated %02d:%02d:%02d",
                 ti->tm_hour, ti->tm_min, ti->tm_sec);

        /* ---- 6. Battery + temperature (unchanged) ---- */
        {
            int pct = battery_get_percentage();
            if (pct != 255) battery_pct = pct;
        }
        char temp_str[8] = {0};
        const char *temp_ptr = NULL;
        if (weather_get_temp_str(temp_str, sizeof(temp_str))) {
            temp_ptr = temp_str;
        }

        /* ---- 7. NEW: Build page indicator string ---- */
        char page_str[12] = {0};
        const char *page_ptr = NULL;
        if (s_page_count > 1) {
            snprintf(page_str, sizeof(page_str), "Page %d/%d",
                     page + 1, s_page_count);
            page_ptr = page_str;
        }

        /* ---- 8. Compute seconds until next wall-clock boundary ---- */
        int sec = ti->tm_sec;
        int next_seconds = s_refresh_interval - (sec % s_refresh_interval);

        /* ---- 9. Render ---- */
        render_dashboard(time_str, temp_ptr, last_updated, battery_pct,
                         page_ptr,
                         s_route_buf[buf_idx][page],
                         s_pages[page].count);

        /* ---- 10. Sleep until next boundary ---- */
        vTaskDelay(pdMS_TO_TICKS(next_seconds * 1000));
    }
}
```

### 5.5 Render-on-switch behaviour

> **[SUPERSEDED]** — Steps 2 and 4 describe the `xTaskNotifyGive` early-wake and a
> page-specific fetch. Current behaviour: the toggle flips `s_active_page` only; the
> current render cycle draws the new page from the already-fetched double buffer
> (≤ one fetch interval old). No fetch wake.

On button press:
1. `s_active_page` toggles immediately (atomic store).
2. `xTaskNotifyGive` wakes `eta_fetch_task` — fresh fetch for the new page starts within
   milliseconds.
3. The **current** render cycle (already running or about to run) renders the new page
   using **last-known-good ETAs** from the buffer for that page (or `--` if never fetched
   / expired >3 min ago).
4. The next render (within `refresh_seconds`, default 10 s) picks up the freshly-fetched
   ETAs.

Worst-case perceived latency on switch for fresh ETAs: ~10 s (next wall-clock boundary).
But the page content (route numbers, destinations, stops) appears instantly because those
are static config fields populated at boot.

---

## 6. Edge cases & failure modes

| Case | Behaviour |
|---|---|
| `pages` absent, `routes` present (legacy JSON) | Single-page mode. `s_page_count = 1`. Button press is a no-op (`s_page_count > 1` guard). Footer hides "Page X/2". |
| Page 2 present but `routes` empty | `s_page_count = 2`. Toggling to page 2 shows 3 blank rows. Footer shows "Page 2/2". |
| Page 2 has 1 or 2 routes | Missing rows render blank (no divider, no text). Present rows render normally. |
| Page 2 has >3 routes | Excess entries logged and skipped at `route_config_load_pages()`. First 3 are used. |
| Button press during Wi-Fi disconnect | Toggle still works; fetch will succeed once `WIFI_EVENT_STA_DISCONNECTED` handler reconnects. Existing recovery logic unchanged. |
| Button press during fetch in progress | **[SUPERSEDED]** Notification latches. *(No notification exists today — the toggle only flips `s_active_page`; the in-flight fetch completes and publishes all pages.)* |
| Rapid button presses (bounce / double-tap) | `button_consume_presses()` collapses all presses since last poll into one non-zero count → single toggle per render cycle. Bounce within ~10 ms collapses to one toggle. Two deliberate presses within one `refresh_seconds` window (10 s) → still one toggle (second press arrives after consume-reset, toggles back). **Acceptable**: matches "manual toggle" intent. |
| Page switch render before first fetch of page 2 | **[SUPERSEDED]** All ETAs show `--` (sentinel from boot init). *(With fetch-all, page 2 is fetched from the first cycle — no page-specific first fetch.)* |
| Device boots, user never presses KEY | **[SUPERSEDED]** Page 2 routes never fetched (power saving). *(With fetch-all, all pages are fetched every cycle regardless of the visible page.)* |
| `s_page_count == 1` (legacy or page 2 absent) | `button_init()` still called (driver reusable), but presses are no-ops. Footer hides page indicator. |

---

## 7. Files to change

| File | Change |
|---|---|
| `spiffs_data/routes.json` | **Migrate** to `pages: [ {routes: [...]}, {routes: [...]} ]` structure. Page 2 = placeholder (duplicated from page 1) — user will replace later. |
| `main/route_config.h` | Add `page_config_t`, `MAX_PAGES`, `ROUTES_PER_PAGE`. Replace `route_config_load()` with `route_config_load_pages()`. |
| `main/route_config.c` | Refactor per-route parsing into `parse_one_route()`. Parse `"pages"` array (or legacy `"routes"` fallback). Fill `page_config_t[]`. |
| `main/button.h` | **New** — GPIO18 KEY button driver API. |
| `main/button.c` | **New** — ISR-based press counter, atomic consume. |
| `main/display.h` | Update `render_footer()` and `render_dashboard()` signatures (add `page_indicator_str`, `route_count`). |
| `main/display.c` | `render_footer()`: draw `page_indicator_str` 10 px after `updated_str`. `render_dashboard()`: iterate only `route_count` rows. |
| `main/main.c` | Per-page buffers, `s_active_page` atomic, button init, page-toggle in `display_task`. **[SUPERSEDED]** `xTaskNotifyWait` in `eta_fetch_task`, fetch only active page *(now fetch-all + `vTaskDelay`, see plan-fetch-all-oos.md)*. |
| `main/CMakeLists.txt` | Add `button.c` to SRCS. `esp_driver_gpio` already in REQUIRES. |
| `design.md` | §3 rule 4 (Footer band): add "Page X/2" indicator as third element, 10 px after Updated text, hidden in single-page mode. §3 screen structure ASCII diagram: update footer line. §7 checklist: add verification item. |
| `CLAUDE.md` | New "Multi-Page Display" section: JSON schema, fetch strategy (visible page only), GPIO18 ownership note (page toggle wins; sleep plan must be reworked), button driver API. |
| `HANDOFF.md` | Update §1 (Last Completed Step), §2 (Files Touched), §3 (Build Status), §4 (Known Issues — note GPIO18 conflict with pending sleep plan). |
| `PRD.md` | Add FR for second page + page indicator. Note in §10 Open Decisions: GPIO18 now claimed by page-toggle; sleep plan needs rework. |

---

## 8. Build & verification

1. `idf.py build` — expect PASS. Binary size increase: negligible (button driver ~1 KB,
   per-page buffers add `2 pages × 3 routes × sizeof(route_data_t)` ≈ 200 bytes).
2. Flash to device.
3. **Functional tests**:
   - Boot → page 1 renders, footer shows "Page 1/2".
   - Press KEY → page 2 renders within 1 render cycle. Footer shows "Page 2/2".
   - **[SUPERSEDED]** Page 2 ETAs show last-known-good or `--` initially, then populate within ~30 s (fetch wake + HTTP round-trip). *(Current: both pages are fetched every cycle from boot, so page 2 ETAs are already fresh on first toggle.)*
   - Press KEY again → back to page 1, ETAs are last-known-good, refresh within ~30 s.
   - Hold off pressing for 5 min → display stays on whichever page was last selected.
     No auto-rotate.
4. **Config tests**:
   - Remove `"pages"` from routes.json, use legacy `"routes"` → single-page mode, button
     no-op, no page indicator.
   - Page 2 with 2 routes → page 2 shows 2 rows + 1 blank row.
   - Page 2 with 0 routes → page 2 shows 3 blank rows, footer still "Page 2/2".
5. **Power sanity**: Current draw should be similar to today (3 routes/cycle, no extra
   fetches when page 2 is not viewed).

---

## 9. Critical review notes

### 9.1 GPIO18 conflict with pending sleep plan

The pending `docs/plan-display-sleep-button-wake.md` also wants GPIO18. Per user decision,
**page toggle wins GPIO18 short-press**. When the sleep plan is implemented, it must be
reworked to either:
- Add a long-press vs short-press discriminator in `button.c` (short → page toggle,
  long → display wake), or
- Reassign display-sleep-wake to a different physical input (none currently available).

This plan creates `button.c` with press-count semantics that can be extended (press
duration measurement) without breaking the page-toggle consumer.

### 9.2 Double-buffer per page — memory cost

`s_route_buf[2][2][3]` = 12 `route_data_t` entries vs. the current 6. Each
`route_data_t` is ~40 bytes (3 `time_t` + 3 pointers). Total increase: ~240 bytes.
Negligible against the 8 MB PSRAM-backed heap.

### 9.3 Fetch task early-wake on page switch — no race

> **[SUPERSEDED]** — The early-wake mechanism described here was removed in Rev 2 of
> `docs/plan-fetch-all-oos.md` (D9). There is no `xTaskNotifyGive` on page toggle today.

`s_active_page` is read by `eta_fetch_task` at the top of each cycle (single snapshot).
If `display_task` toggles the page mid-fetch, the current fetch completes for the old
page (harmless — writes to the inactive buffer's old-page slot), and the next cycle
fetches the new page. The `xTaskNotifyGive` ensures the next cycle starts immediately
rather than after the full 30 s delay.

### 9.4 `xTaskNotifyWait` semantics

> **[SUPERSEDED]** — `xTaskNotifyWait` was replaced by a plain `vTaskDelay` in
> `eta_fetch_task` (Rev 2 of plan-fetch-all-oos.md); the notification value can no longer
> leak because no notifications are ever sent. Retained below as historical record.

`xTaskNotifyWait(0, 0, NULL, timeout)`:
- First arg `0` (notify bits to clear on entry): none — we don't use bit-based notify.
- Second arg `0` (notify bits to clear on exit): none.
- Third arg `NULL`: don't receive the notification value (we only care about the wake).
- Fourth arg `timeout`: block for up to `delay_ms`, or until notified.

If a notification arrives before the call (latched), returns immediately. If it arrives
during the block, wakes immediately. This is exactly the "early-wake from delay" pattern
needed for responsive page switching.

### 9.5 Page indicator hidden in single-page mode

When `s_page_count == 1`, `page_ptr` is `NULL`, and `render_footer()` skips drawing the
indicator. The footer reverts to the current two-element layout (Updated + Battery).
This means legacy users (who haven't migrated routes.json) see no visual change.

### 9.6 No change to weather, battery, NTP, Wi-Fi recovery

All existing subsystems are untouched. Weather fetch piggybacks on `eta_fetch_task` every
20th cycle regardless of which page is active. Battery sampling happens after each fetch
cycle. NTP resync at 06:00 is independent. Wi-Fi disconnect recovery is unchanged.

---

## 10. Implementation order

1. `main/button.h` + `main/button.c` (standalone, testable in isolation).
2. `main/route_config.h` + `main/route_config.c` (refactor parser, add pages support).
3. `spiffs_data/routes.json` (migrate to new format with placeholder page 2).
4. `main/display.h` + `main/display.c` (footer indicator, route_count in dashboard).
5. `main/main.c` (per-page buffers, toggle logic, fetch gating, notify).
6. `main/CMakeLists.txt` (add `button.c`).
7. Build → flash → functional tests (§8).
8. Docs: `design.md`, `CLAUDE.md`, `HANDOFF.md`, `PRD.md`.

Each step is independently compilable (with stubs where needed) to keep the build green
between steps.
