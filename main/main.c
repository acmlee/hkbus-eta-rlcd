/**
 * @file main.c
 * @brief Entry point for hk-bus-eta-rlcd — ETA dashboard on ESP32-S3-RLCD.
 *
 * Two-task design:
 *   - eta_fetch_task:  fetches ETA for ALL configured routes (every page)
 *                       each cycle into a double-buffered shared buffer,
 *                       toggles Wi-Fi PS.  Cycle interval is configurable
 *                       (~30 s default, ±10% random jitter) and relaxes to
 *                       ~300 s during the out-of-service night window.
 *   - display_task:    renders at wall-clock boundaries, handles
 *                       daily NTP resync, KEY button page toggle.
 *
 * Flow:
 *   1. Init NVS, SPIFFS, display, Wi-Fi, SNTP, button (sequential at boot)
 *   2. Load page config from routes.json
 *   3. Create eta_fetch_task + display_task (higher priority)
 *   4. app_main deletes itself; tasks run forever
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_random.h"
#include "esp_pm.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif_sntp.h"

#include "display.h"
#include "eta_fetcher.h"
#include "route_config.h"
#include "battery.h"
#include "weather_hko.h"
#include "button.h"
#include "http_util.h"
#include "rtc_pcf85063.h"
#include "esp_timer.h"
/* cJSON parsing is internal to eta_fetcher.c */

static const char *TAG = "hkbus";

/* Event group bits for Wi-Fi connection */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

/* Epoch threshold above which the SNTP clock is considered valid
 * (2023-11-14).  Before this, localtime() returns a 1970-era time.
 * Shared by ntp_wait_for_sync() and the header date build. */
#define EPOCH_SYNC_THRESHOLD 1700000000UL

/* Track reconnection attempts for diagnostic logging */
static int s_reconnect_count = 0;

/* Wi-Fi connection state — shared between wifi_event_handler (ISR-like
 * context) and display_task.  Spinlock protects cross-core visibility
 * on the dual-core ESP32-S3 (no hardware cache coherency). */
static portMUX_TYPE s_wifi_lock = portMUX_INITIALIZER_UNLOCKED;
static bool         s_wifi_connected = false;

/* Clock trust gate (docs/plan-rtc-pcf85063.md §11).  True when the
 * system clock is trusted: an RTC with year >= 2026 was restored at
 * boot, or the first successful SNTP sync has fired.  Until true, the
 * header date/time, the footer "Updated" timestamp, and the ETA display
 * are suppressed (and eta_fetch_task does not fetch).  Word-sized
 * volatile — atomic on ESP32-S3, mirrors the s_active_page pattern. */
static volatile bool s_clock_trusted = false;

/* Power management (plan-battery-optimizations.md Phase 1/2): holds the CPU
 * at max frequency during the ETA fetch burst.  With PM enabled the SoC
 * light-sleeps whenever both tasks are blocked, and DFS drops the CPU to
 * 80 MHz when idle — but the TLS handshakes are ~2× slower at 80 MHz, so the
 * fetch burst takes this lock to stay at 160 MHz.  Created in app_main,
 * acquired/released by eta_fetch_task around the fetch loop. */
static esp_pm_lock_handle_t s_fetch_cpu_lock = NULL;

/* Boot-time "stay awake for flashing" lock (plan-battery-optimizations.md):
 * light sleep powers down the USB-Serial-JTAG, so a freshly-booted device
 * must stay awake long enough to be flashed via the auto-reset.  Acquired in
 * app_main and released by usb_boot_grace_expired() after USB_BOOT_GRACE_US.
 * Beyond the grace, the built-in USJ connection monitor
 * (CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION) holds its own NO_LIGHT_SLEEP lock
 * while a USB host is attached and releases it when the host detaches. */
#define USB_BOOT_GRACE_US (30LL * 1000LL * 1000LL)   /* 30 s */
static esp_pm_lock_handle_t s_boot_grace_lock = NULL;

/* ------------------------------------------------------------------
 * Double-buffered shared ETA data (per-page)
 *
 * Two buffers of MAX_PAGES pages of ROUTES_PER_PAGE route_data_t each.
 * eta_fetch_task writes to the inactive buffer for ALL pages (every route
 * in routes.json, regardless of the visible page), then atomically flips
 * s_active_buf_idx (0 or 1).
 * display_task reads s_active_buf_idx and s_active_page once per
 * render cycle and uses those buffers for the entire render.
 * No mutex/semaphore is needed because:
 *   - The active index and page are word-sized ints — aligned 32-bit
 *     writes on ESP32-S3 are naturally atomic (no tearing).
 *   - display_task reads the index once at the top of its loop, then
 *     reads from the same buffer throughout; a concurrent flip by
 *     fetch_task only affects the *next* render cycle.
 *   - There is no reader/writer contention on the same buffer at the
 *     same time: fetch_task writes the inactive buffer while
 *     display_task reads the active buffer, and the flip itself is a
 *     single-copy-atomic store.
 * ----------------------------------------------------------------*/
static route_data_t s_route_buf[2][MAX_PAGES][ROUTES_PER_PAGE];
static volatile int  s_active_buf_idx = 0;   /* 0 or 1, word-sized atomic */
static volatile int  s_active_page    = 0;   /* 0 or 1, word-sized atomic */

/* Page config — loaded once at boot, shared by both tasks */
static page_config_t s_pages[MAX_PAGES];
static int           s_page_count = 0;

/* Display refresh interval (seconds) — read from routes.json once,
 * validated to divide 60 evenly for clean wall-clock boundaries. */
static int s_refresh_interval = 10;

/* ETA fetch intervals + out-of-service window — read from routes.json
 * once at boot (plan-fetch-all-oos.md §4).  In service the ETA fetch
 * runs every s_fetch_interval_s (~30 s default); out of service (inside
 * [s_oos_start_min, s_oos_end_min) AND all routes empty) it relaxes to
 * s_oos_fetch_interval_s (~300 s default).  Unlike refresh_seconds these
 * are NOT render-aligned — the divide-60 constraint does not apply. */
static int s_fetch_interval_s     = 30;
static int s_oos_fetch_interval_s = 300;
static int s_oos_start_min        = 60;    /* 01:00 */
static int s_oos_end_min          = 330;   /* 05:30 */

/* Weather piggyback cadence — time-based, state-independent: at the
 * in-service 30 s fetch interval this fires every ~20 cycles (~600 s);
 * during out-of-service (300 s interval) every ~2 cycles.  The weather
 * request rides an already-awake radio window (the OOS cycle wakes the
 * radio for the ETA fetches anyway), so the OOS power-saving is not
 * undermined. */
#define WEATHER_FETCH_INTERVAL_S 600

/* ------------------------------------------------------------------
 * Wi-Fi station — connect with retries using EventGroup
 * ----------------------------------------------------------------*/
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    EventGroupHandle_t events = (EventGroupHandle_t)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_reconnect_count++;
        ESP_LOGW(TAG, "Wi-Fi disconnected, reconnecting (attempt %d)...",
                 s_reconnect_count);
        taskENTER_CRITICAL(&s_wifi_lock);
        s_wifi_connected = false;
        taskEXIT_CRITICAL(&s_wifi_lock);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
        s_reconnect_count = 0;
        taskENTER_CRITICAL(&s_wifi_lock);
        s_wifi_connected = true;
        taskEXIT_CRITICAL(&s_wifi_lock);
        xEventGroupSetBits(events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(const char *ssid, const char *pass)
{
    /* 1. Initialise default netif and event loop */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    /* 2. Create event group */
    EventGroupHandle_t wifi_events = xEventGroupCreate();

    /* 3. Init Wi-Fi in station mode */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* 4. Register event handlers (pass event group as arg) */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, wifi_events, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, wifi_events, NULL));

    /* 5. Configure station */
    wifi_config_t wifi_cfg = { 0 };
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    /* Listen interval (AP beacon periods, 0 = default 3): only used when the
     * station is in WIFI_PS_MAX_MODEM, which the firmware applies during the
     * out-of-service night window (plan-battery-optimizations.md Phase 4) so
     * the radio wakes every 5 beacons instead of every DTIM.  Irrelevant to
     * WIFI_PS_MIN_MODEM, the in-service resting state. */
    wifi_cfg.sta.listen_interval = 5;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));

    /* 6. Start and connect with retry loop */
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Wi-Fi started, connecting to %s", ssid);

    int retries = 0;
    const int MAX_RETRIES = 5;
    EventBits_t bits;

    for (;;) {
        /* Clear bits before each connect attempt */
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        esp_wifi_connect();

        /* Block until connected or retry limit reached */
        bits = xEventGroupWaitBits(wifi_events,
                                   WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                   pdFALSE, pdFALSE,
                                   pdMS_TO_TICKS(2000));

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Wi-Fi connected");
            break;
        }

        retries++;
        ESP_LOGW(TAG, "Wi-Fi connect attempt %d/%d failed", retries, MAX_RETRIES);
        if (retries >= MAX_RETRIES) {
            ESP_LOGE(TAG, "Wi-Fi connect failed after %d retries", MAX_RETRIES);
            break;
        }
    }

    /* 7. Clean up event handlers (keep event group alive for the task) */
    /* We keep the event group to avoid dangling pointers; handlers stay registered */
    (void)bits;
}

/* ------------------------------------------------------------------
 * SNTP — Asia/Hong_Kong (UTC+8, HKT) using stdtime.gov.hk
 *
 * ntp_wait_for_sync() is the shared wait loop reused by both the
 * boot-time sync and the daily resync.
 * ----------------------------------------------------------------*/

static void ntp_wait_for_sync(void)
{
    int retries = 0;
    const int MAX_RETRIES = 10;
    time_t now = 0;
    while (time(&now) < EPOCH_SYNC_THRESHOLD && retries < MAX_RETRIES) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        retries++;
    }

    if (now >= EPOCH_SYNC_THRESHOLD) {
        struct tm *ti = localtime(&now);
        ESP_LOGI(TAG, "Time synced: %02d:%02d:%02d",
                 ti->tm_hour, ti->tm_min, ti->tm_sec);
    } else {
        ESP_LOGW(TAG, "SNTP sync timeout (%ds), proceeding anyway", retries);
    }
}

/* SNTP sync callback — invoked by the SNTP task on every successful
 * time sync (boot sync, periodic sync, and the daily 06:00 resync).
 * Pushes the freshly-synced stdtime.gov.hk time into the PCF85063 RTC,
 * which is how the built-in clock is kept accurate across power cycles.
 * Also flips the clock-trust gate: from this moment the header
 * date/time and ETAs are shown (eta_fetch_task picks it up within
 * 500 ms — no explicit notification needed). */
static void sntp_sync_cb(struct timeval *tv)
{
    (void)tv;
    s_clock_trusted = true;
    rtc_pcf85063_store_system_time();
}

static void time_sync_init(void)
{
    /* 1. Set timezone BEFORE SNTP so timestamps are interpreted in HKT */
    setenv("TZ", "HKT-8", 1);
    tzset();

    /* 2. Restore wall clock from the onboard PCF85063 RTC so the header
     * shows the correct time immediately — no `-- --- (---)` wait, and
     * the clock still works if Wi-Fi/SNTP is unavailable.  Only an RTC
     * with year >= 2026 is trusted (plan-rtc-pcf85063.md §11): a lower
     * year means the clock is presumed severely unsynced and stays
     * hidden (date/time/ETAs) until the first successful SNTP sync. */
    if (rtc_pcf85063_restore_system_time()) {
        ESP_LOGI(TAG, "Boot clock restored from RTC (pre-SNTP)");
        s_clock_trusted = true;
    } else {
        ESP_LOGI(TAG, "Clock untrusted at boot — date/time/ETAs hidden until first SNTP sync");
    }

    /* 3. Configure SNTP. sync_cb pushes every successful sync into the
     * RTC — boot sync, periodic syncs, and the daily 06:00 resync —
     * which is how the built-in clock is *updated* (never "reset" to a
     * hardcoded value). */
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("stdtime.gov.hk");
    cfg.sync_cb = sntp_sync_cb;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&cfg));

    /* 4. Block until time > 1700000000 (Jan 2024) or 10 s timeout.
     * With a valid RTC this returns immediately. */
    ntp_wait_for_sync();
}

/**
 * @brief Trigger a one-shot NTP resync (daily clock-drift correction).
 *
 * Deinits and re-inits the SNTP service to force a fresh query, then
 * waits up to ~10 s for the sync to complete.  Logs the old vs new
 * time if measurable drift was corrected.  Never crashes on timeout —
 * just logs a warning and returns.
 *
 * Caller (display_task) is responsible for the "once per day" guard
 * via the tm_yday tracking variable.
 */
static void ntp_resync(void)
{
    ESP_LOGI(TAG, "Daily NTP resync started");

    time_t old_time = time(NULL);

    /* Restart SNTP to force a fresh query */
    esp_netif_sntp_deinit();

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("stdtime.gov.hk");
    cfg.sync_cb = sntp_sync_cb;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NTP resync: esp_netif_sntp_init failed (0x%x), skipping", err);
        return;
    }

    ntp_wait_for_sync();

    time_t new_time = time(NULL);
    if (new_time >= 1700000000 && old_time >= 1700000000) {
        double drift = difftime(new_time, old_time);
        if (drift > 1.0 || drift < -1.0) {
            ESP_LOGI(TAG, "NTP resync corrected clock by %.0f seconds", drift);
        } else {
            ESP_LOGI(TAG, "NTP resync complete (drift < 1s)");
        }
    }
}

/* ETA parsing is handled internally by eta_fetcher.c */

/* ------------------------------------------------------------------
 * eta_fetch_task — ETA fetch loop, one task
 *
 * Runs every fetch interval (vTaskDelay): ~30 s in service, ~300 s out
 * of service (plan-fetch-all-oos.md §4).  Disables Wi-Fi modem-sleep
 * before fetching, re-enables it after.  Fetches ALL configured routes
 * (every page — regardless of the visible page) so a page toggle never
 * shows empty/stale data and needs no immediate refetch.  Writes fresh
 * ETA values into the currently inactive double-buffer, then atomically
 * flips the active buffer index so display_task sees the new data on
 * its next render.
 *
 * Priority: tskIDLE_PRIORITY+2 (lower than display_task).
 * ----------------------------------------------------------------*/
static void eta_fetch_task(void *arg)
{
    eta_entry_t eta_buf[3];

    /* Weather cadence — time-based 600 s (WEATHER_FETCH_INTERVAL_S):
     * fetch when due, state-independent.  Initialised to 0 so the first
     * cycle fetches immediately on boot — no 10-min wait for initial
     * temperature (parity with the old "start at 20" counter). */
    static time_t s_last_weather_epoch = 0;

    /* Out-of-service state (plan-fetch-all-oos.md §4.2) — local to this
     * task, used only to log transitions.  The display needs no
     * knowledge of it: all-empty ETAs render "--" naturally. */
    static bool s_oos = false;

    /* Clock-trust retry: the default SNTP sync interval is 1 hour, so a
     * single failed boot sync would otherwise leave the clock untrusted —
     * and every ETA hidden — for a very long time.  Force a fresh SNTP
     * query every 60 s while untrusted. */
    static time_t s_last_ntp_retry = 0;

    (void)arg;

    /* Ensure Wi-Fi PS is in a known state before the loop */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

    while (1) {
        ESP_LOGD(TAG, "Fetch cycle start (all routes, %d pages)", s_page_count);

        /* Clock-trust gate (plan-rtc-pcf85063.md §11): while the clock
         * is untrusted (no RTC year >= 2026, no SNTP sync yet), do not
         * fetch — ETA epochs are meaningless against a 1970-era clock
         * and the display must stay "--".  Poll every 500 ms; the first
         * successful SNTP sync flips s_clock_trusted in sntp_sync_cb.
         * Battery/weather piggybacks are deferred with the fetch (user
         * decision D2). */
        if (!s_clock_trusted) {
            /* Force a fresh SNTP query periodically (60 s) in case the
             * boot-time sync failed — otherwise the clock stays untrusted
             * (and ETAs stay hidden) until the next hourly SNTP poll. */
            time_t now_t;
            time(&now_t);
            if (now_t - s_last_ntp_retry >= 60) {
                s_last_ntp_retry = now_t;
                ESP_LOGW(TAG, "Clock still untrusted — forcing NTP resync");
                ntp_resync();
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        /* Disable modem-sleep so HTTP requests are not delayed by
         * radio power-state transitions. */
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

        /* Hold the CPU at max frequency for the fetch burst
         * (plan-battery-optimizations.md Phase 2): DFS otherwise drops the
         * CPU to 80 MHz when idle, slowing the TLS handshakes ~2×. */
        if (s_fetch_cpu_lock != NULL) {
            esp_err_t lock_err = esp_pm_lock_acquire(s_fetch_cpu_lock);
            if (lock_err != ESP_OK) {
                ESP_LOGW(TAG, "esp_pm_lock_acquire failed (0x%x)", lock_err);
            }
        }

        /* Determine which buffer is inactive (the one display_task
         * is NOT currently reading).  The read of s_active_buf_idx
         * here is safe: fetch_task is the only writer, and the
         * window between this read and the write at the bottom of
         * the loop is the entire fetch cycle — display_task may
         * flip the active index via its own read, but that only
         * affects the *next* loop iteration. */
        int inactive = 1 - s_active_buf_idx;

        /* Fetch ETA for ALL pages' routes into the inactive buffer —
         * every route in routes.json regardless of the visible page,
         * so a page toggle never shows empty/stale data. */
        time_t now;
        time(&now);
        for (int p = 0; p < s_page_count; p++) {
            for (int i = 0; i < s_pages[p].count && i < ROUTES_PER_PAGE; i++) {
                int n = fetch_eta(&s_pages[p].routes[i], eta_buf, 3);
                if (n > 0) {
                    s_route_buf[inactive][p][i].eta1 = (n >= 1) ? eta_buf[0].eta_epoch : (time_t)-1;
                    s_route_buf[inactive][p][i].eta2 = (n >= 2) ? eta_buf[1].eta_epoch : (time_t)-1;
                    s_route_buf[inactive][p][i].eta3 = (n >= 3) ? eta_buf[2].eta_epoch : (time_t)-1;
                } else {
                    /* Fetch failed — preserve last-known-good ETAs from the
                     * active buffer, but expire to "--" if the ETA timestamp
                     * is more than 3 minutes (180 s) in the past.  Comparing
                     * against the ETA itself (not the fetch time) naturally
                     * handles both "future ETA still valid" and "past ETA
                     * should be cleared" without needing a separate fetch
                     * timestamp. */
                    int active = s_active_buf_idx;
                    time_t e1 = s_route_buf[active][p][i].eta1;
                    time_t e2 = s_route_buf[active][p][i].eta2;
                    time_t e3 = s_route_buf[active][p][i].eta3;

                    s_route_buf[inactive][p][i].eta1 = (e1 != (time_t)-1 && difftime(now, e1) < 180.0) ? e1 : (time_t)-1;
                    s_route_buf[inactive][p][i].eta2 = (e2 != (time_t)-1 && difftime(now, e2) < 180.0) ? e2 : (time_t)-1;
                    s_route_buf[inactive][p][i].eta3 = (e3 != (time_t)-1 && difftime(now, e3) < 180.0) ? e3 : (time_t)-1;
                }

                /* Refresh static fields (cheap defensive copy) */
                s_route_buf[inactive][p][i].route_num = s_pages[p].routes[i].route;
                s_route_buf[inactive][p][i].dest_zh   = s_pages[p].routes[i].dest_zh;
                s_route_buf[inactive][p][i].stop_zh   = s_pages[p].routes[i].stop_zh;

                /* Yield to WiFi driver between requests to prevent
                 * interrupt-watchdog timeout */
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }

        /* Re-enable modem-sleep after fetch completes */
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
        if (s_fetch_cpu_lock != NULL) {
            esp_err_t lock_err = esp_pm_lock_release(s_fetch_cpu_lock);
            if (lock_err != ESP_OK) {
                ESP_LOGW(TAG, "esp_pm_lock_release failed (0x%x)", lock_err);
            }
        }

        /* Sample battery during confirmed Wi-Fi-idle window.
         * The 50 ms settle delay is inside battery_sample_if_due().
         * Internal cadence: samples every 2nd call (~60 s in service,
         * ~600 s out of service — cached value, harmless). */
        battery_sample_if_due();

        /* Re-read the clock after the fetch cycle (plan-fetch-all-oos.md
         * §4.2): the OOS window check and the oos_end boundary cap must
         * use the post-fetch time, so a cycle spanning 05:30 restores
         * in-service on this same cycle rather than one cycle later.
         * The ETA-expiry difftime inside the fetch loop used the
         * pre-fetch time — a few seconds' skew there is immaterial. */
        time(&now);

        /* Weather piggyback — time-based 600 s cadence, state-
         * independent: at the 30 s fetch interval that is every ~20
         * cycles; during OOS (300 s interval) every ~2 cycles, so
         * weather stays fresh all night.  The request rides an
         * already-awake radio window (the OOS cycle woke the radio
         * for the ETA fetches anyway), so the 300 s power-saving is
         * not undermined.  Timestamp advances regardless of fetch
         * success (parity with the old cycle counter). */
        if (now - s_last_weather_epoch >= WEATHER_FETCH_INTERVAL_S) {
            s_last_weather_epoch = now;
            weather_fetch_once();
        }

        /* Drop the per-host reuse connections (plan-battery-optimizations.md
         * Phase 3): never reuse across cycles — servers close idle
         * connections after a few seconds, so each cycle deliberately starts
         * with a fresh handle and one TLS handshake per host. */
        http_close_reuse_clients();

        /* Out-of-service state machine (plan-fetch-all-oos.md §4.2):
         * OOS when the wall clock is inside [s_oos_start_min,
         * s_oos_end_min) AND every configured route has all three ETA
         * slots == -1 (blank rows on partial pages are not routes and
         * must not count).  Any non-empty ETA keeps the device in
         * service even inside the night window.  Evaluated against the
         * just-written inactive buffer (post-fetch, post-preservation),
         * i.e. exactly what display_task will render next. */
        bool all_empty = true;
        for (int p = 0; p < s_page_count && all_empty; p++) {
            for (int i = 0; i < s_pages[p].count && i < ROUTES_PER_PAGE; i++) {
                if (s_route_buf[inactive][p][i].eta1 != (time_t)-1 ||
                    s_route_buf[inactive][p][i].eta2 != (time_t)-1 ||
                    s_route_buf[inactive][p][i].eta3 != (time_t)-1) {
                    all_empty = false;
                    break;
                }
            }
        }

        struct tm *ti = localtime(&now);
        int now_min = ti->tm_hour * 60 + ti->tm_min;
        bool in_window = (now_min >= s_oos_start_min) && (now_min < s_oos_end_min);
        bool oos = in_window && all_empty;

        if (oos != s_oos) {
            s_oos = oos;
            if (oos) {
                ESP_LOGW(TAG, "Out of service — fetch interval %d s",
                         s_oos_fetch_interval_s);
            } else {
                ESP_LOGW(TAG, "In service — fetch interval %d s",
                         s_fetch_interval_s);
            }
        }

        /* Night-time deep modem sleep (plan-battery-optimizations.md
         * Phase 4): while OOS the fetch interval is already 300 s, so the
         * radio rests in WIFI_PS_MAX_MODEM (waking only every listen_interval
         * beacons — set at connect time) instead of every DTIM.  Re-asserted
         * every OOS cycle because the fetch above restored MIN_MODEM.  The
         * OOS → in-service transition needs no explicit restore: the next
         * cycle's fetch path re-asserts MIN_MODEM anyway. */
        if (oos) {
            esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
            if (ps_err != ESP_OK) {
                ESP_LOGW(TAG, "wifi MAX_MODEM set failed (0x%x)", ps_err);
            }
        }

        /* Atomic flip: publish the newly-filled buffer.
         * This is a single aligned word-sized write — naturally
         * atomic on ESP32-S3.  display_task may read either the
         * old or new index on its next render, but never a torn
         * value. */
        s_active_buf_idx = inactive;

        /* Arm the wait: active interval ±10 % random jitter to avoid
         * thundering-herd alignment with other clients.  While OOS the
         * wait is capped at the oos_end boundary so 05:30 always
         * triggers a wake — a 300 s wait armed at 04:50 must not run to
         * 09:50 (the boundary wake re-evaluates and restores the
         * in-service cadence immediately).  Plain vTaskDelay: the fetch
         * cadence is independent of page switches — every page is
         * fetched each cycle regardless of the visible page. */
        int interval_ms = (oos ? s_oos_fetch_interval_s : s_fetch_interval_s) * 1000;
        int half = interval_ms / 10;
        int jitter = (int)(esp_random() % (2 * half + 1)) - half;
        int delay_ms = interval_ms + jitter;

        if (oos) {
            int now_total_ms = (ti->tm_hour * 3600 + ti->tm_min * 60 + ti->tm_sec) * 1000;
            int ms_to_end = s_oos_end_min * 60000 - now_total_ms;
            if (ms_to_end < delay_ms) delay_ms = ms_to_end;
            if (delay_ms <= 0) delay_ms = 10;   /* just past boundary: re-eval now */
        }

        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

/* ------------------------------------------------------------------
 * Header date build — "DD MMM (DDD)" (e.g. " 9 Aug (Sun)").
 *
 * Single-digit days render a space in the tens slot (" 9", not "09")
 * so the ones digit stays at a stable x-position as the date counts
 * up.  Month/weekday are English abbreviations ("Aug", "Sun") from
 * the default C locale.  Before the SNTP clock is valid, fill the
 * buffer with a placeholder instead of a 1970-era date.
 * ----------------------------------------------------------------*/
static void build_header_date_str(time_t now, char *buf, size_t len)
{
    if (now < (time_t)EPOCH_SYNC_THRESHOLD) {
        snprintf(buf, len, "-- --- (---)");
        return;
    }

    struct tm *ti = localtime(&now);
    char mdw[16];   /* e.g. "Aug (Sun)" */
    strftime(mdw, sizeof(mdw), "%b (%a)", ti);
    snprintf(buf, len, (ti->tm_mday < 10) ? " %d %s" : "%d %s",
             ti->tm_mday, mdw);
}

/* ------------------------------------------------------------------
 * display_task — render loop, one task
 *
 * Runs at wall-clock boundaries.  Each tick:
 *   1. Reads current time, builds HH:MM header string
 *   2. Checks daily NTP resync condition (06:00, once per day) and
 *      triggers it if due
 *   3. Consumes button presses — toggles page if multi-page mode
 *   4. Snaps the active buffer index + active page (single atomic reads)
 *   5. Builds "Updated HH:MM:SS" footer string + "Page X/Y" indicator
 *   6. Computes seconds until next wall-clock boundary
 *   7. Calls render_dashboard() with the active buffer
 *   8. Sleeps until the next boundary
 *
 * Priority: tskIDLE_PRIORITY+3 (higher than eta_fetch_task), because
 * render timing alignment is the more time-critical concern.
 * ----------------------------------------------------------------*/

/* ---- Boot-time USB flashing grace (plan-battery-optimizations.md) ----
 * Releases the boot-grace NO_LIGHT_SLEEP lock USB_BOOT_GRACE_US after boot.
 * From then on the USJ connection monitor (CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION)
 * keeps the device awake only while a USB host is actually attached.  Runs in
 * the esp_timer task context (not ISR), so esp_pm_lock_release is safe. */
static void usb_boot_grace_expired(void *arg)
{
    (void)arg;
    if (s_boot_grace_lock != NULL) {
        esp_pm_lock_release(s_boot_grace_lock);
        ESP_LOGI(TAG, "USB boot grace expired — light sleep enabled (hold BOOT+RST to enter download mode)");
    }
}

static void display_task(void *arg)
{
    char last_updated[32];
    char time_str[6];
    char date_str[24];

    /* Track the last day (tm_yday) a resync was attempted.
     * -1 ensures the first 06:00 after boot always triggers. */
    int last_resync_yday = -1;

    /* Battery percentage — read asynchronously by eta_fetch_task
     * during Wi-Fi-idle windows.  battery_pct starts at 255 (unknown)
     * and is displayed as "Battery:  --%" until the first successful
     * filtered reading is available. */
    static int battery_pct = 255;

    (void)arg;

    while (1) {
        /* ---- 1. Read wall-clock time ---- */
        time_t now;
        time(&now);
        struct tm *ti = localtime(&now);

        /* Build HH:MM header + date label */
        snprintf(time_str, sizeof(time_str), "%02d:%02d",
                 ti->tm_hour, ti->tm_min);
        build_header_date_str(now, date_str, sizeof(date_str));

        /* Clock-trust gate: hide the header date/time until the clock is
         * trusted (RTC year >= 2026 or first SNTP sync).  Pre-sync the
         * epoch is 1970, which would render "08:00" and a 1970 date. */
        if (!s_clock_trusted) {
            snprintf(time_str, sizeof(time_str), "----");
            snprintf(date_str, sizeof(date_str), "-- --- (---)");
        }

        /* ---- 2. Daily NTP resync at 06:00 ---- */
        /* Trigger once per day at 06:xx.  Uses tm_yday to ensure the
         * resync fires exactly once, not every tick during the
         * 06:00 hour.  The guard is updated *after* the attempt
         * regardless of success/failure, so a failed attempt does not
         * retry every tick for the rest of the hour — but tomorrow's
         * 06:00 will still trigger one new attempt (since tm_yday
         * will differ). */
        if (ti->tm_hour == 6 && ti->tm_yday != last_resync_yday) {
            ntp_resync();
            last_resync_yday = ti->tm_yday;

            /* Re-read time after resync to update the header strings
             * in case the clock was corrected by more than a minute. */
            time(&now);
            ti = localtime(&now);
            snprintf(time_str, sizeof(time_str), "%02d:%02d",
                     ti->tm_hour, ti->tm_min);
            build_header_date_str(now, date_str, sizeof(date_str));
        }

        /* ---- 3. Snap the active buffer index + page (single atomic reads) ---- */
        int buf_idx = s_active_buf_idx;
        int page    = s_active_page;

        /* ---- 4. Build footer string (timestamp or "Connecting...") ---- */
        taskENTER_CRITICAL(&s_wifi_lock);
        bool wifi_ok = s_wifi_connected;
        taskEXIT_CRITICAL(&s_wifi_lock);
        /* Clock-trust gate: the "Updated HH:MM:SS" timestamp is also
         * suppressed while untrusted — the footer reads "Connecting..."
         * even when Wi-Fi is up, since no valid time exists yet. */
        if (wifi_ok && s_clock_trusted) {
            time(&now);
            ti = localtime(&now);
            snprintf(last_updated, sizeof(last_updated),
                     "Updated %02d:%02d:%02d",
                     ti->tm_hour, ti->tm_min, ti->tm_sec);
        } else {
            snprintf(last_updated, sizeof(last_updated), "Connecting...");
        }

        /* ---- 4a. Read battery percentage (cached — sampled by
         * eta_fetch_task during Wi-Fi-idle windows). ---- */
        {
            int pct = battery_get_percentage();
            if (pct != 255) battery_pct = pct;
        }

        /* ---- 4b. Get temperature string (stale TTL 30 min, hidden if unavailable) ---- */
        char temp_str[8] = {0};
        const char *temp_ptr = NULL;
        if (weather_get_temp_str(temp_str, sizeof(temp_str))) {
            temp_ptr = temp_str;
        }

        /* ---- 4b. Get humidity string (same fetch/TTL; hidden if the station
         * lacks a humidity entry). ---- */
        char hum_str[8] = {0};
        const char *hum_ptr = NULL;
        if (weather_get_humidity_str(hum_str, sizeof(hum_str))) {
            hum_ptr = hum_str;
        }

        /* ---- 4c. Build page indicator string ---- */
        char page_str[32] = {0};
        const char *page_ptr = NULL;
        if (s_page_count > 1) {
            snprintf(page_str, sizeof(page_str), "Page %d/%d",
                     page + 1, s_page_count);
            page_ptr = page_str;
        }

        /* ---- 5. Compute seconds until next wall-clock boundary ---- */
        int sec = ti->tm_sec;
        int next_seconds = s_refresh_interval - (sec % s_refresh_interval);

        /* ---- 6. Render the dashboard ---- */
        render_dashboard(date_str, time_str, temp_ptr, hum_ptr, last_updated,
                         battery_pct, page_ptr,
                         s_route_buf[buf_idx][page],
                         s_pages[page].count);

        /* ---- 7. Poll until next wall-clock boundary, checking button
         *      every 50 ms so page-toggle feels instant (< 100 ms). ----
         *
         * On button press: toggle active page, drain contact-bounce
         * pulses (50 ms + consume), then break out of the polling loop
         * to re-enter the top of while(1) which re-reads time and
         * renders the new page immediately.  No fetch wake is needed —
         * all pages are fetched every cycle by eta_fetch_task, so the
         * toggled page's data is already in the active buffer.
         *
         * On normal timeout: loop back to top and render the next
         * wall-clock frame.
         * ---- */
        {
            int remaining_ms = next_seconds * 1000;
            while (remaining_ms > 0) {
                int chunk = (remaining_ms > 50) ? 50 : remaining_ms;
                vTaskDelay(pdMS_TO_TICKS(chunk));
                remaining_ms -= chunk;

                uint32_t presses = button_consume_presses();
                if (presses > 0 && s_page_count > 1) {
                    s_active_page = 1 - s_active_page;
                    ESP_LOGI(TAG, "Page switch → page %d/%d",
                             s_active_page + 1, s_page_count);

                    /* Drain contact bounces: wait 50 ms, then discard
                     * any late bounce pulses so the next polling window
                     * starts clean. */
                    vTaskDelay(pdMS_TO_TICKS(50));
                    button_consume_presses();

                    break;
                }
            }
        }
    }
}

/* ------------------------------------------------------------------
 * Main application
 * ----------------------------------------------------------------*/
void app_main(void)
{
    /* ---- Init NVS ---- */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase");
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* ---- Init display ---- */
    display_init();

    /* ---- Init KEY button (GPIO18) ---- */
    button_init();

    /* ---- Init battery ADC (before Wi-Fi; ADC1, no conflict) ---- */
    battery_init();

    /* ---- Init Wi-Fi (blocking, up to 5 retries) ---- */
    wifi_init_sta(CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD);

    /* ---- Init onboard PCF85063 RTC (I2C0, SDA=13 SCL=14). If the
     * chip is absent it logs a warning and the firmware stays in
     * pure-SNTP mode — identical behaviour to before. ---- */
    rtc_pcf85063_init();

    /* ---- Init SNTP (blocking up to 10 s; returns immediately when
     * the RTC already restored a valid clock). ---- */
    time_sync_init();

    /* ---- Load page config from SPIFFS ---- */
    s_page_count = route_config_load_pages(s_pages, MAX_PAGES);
    ESP_LOGI(TAG, "Loaded %d pages", s_page_count);

    /* ---- Init weather module (station name from routes.json) ---- */
    weather_init(route_config_get_weather_station());

    /* ---- Read refresh interval from config ---- */
    s_refresh_interval = route_config_get_refresh_interval();
    ESP_LOGI(TAG, "refresh_interval = %d s", s_refresh_interval);

    /* ---- Read ETA fetch intervals + OOS window from config ---- */
    s_fetch_interval_s = route_config_get_fetch_interval();
    s_oos_fetch_interval_s = route_config_get_oos_fetch_interval();
    s_oos_start_min = route_config_get_oos_start_min();
    s_oos_end_min = route_config_get_oos_end_min();
    ESP_LOGI(TAG, "fetch_interval=%d s, oos_fetch_interval=%d s, oos window %02d:%02d-%02d:%02d",
             s_fetch_interval_s, s_oos_fetch_interval_s,
             s_oos_start_min / 60, s_oos_start_min % 60,
             s_oos_end_min / 60, s_oos_end_min % 60);

    /* ---- Initialise double-buffer with static route info ---- */
    /* Both buffers for all pages start with the same static fields
     * (route_num, dest_zh, stop_zh) and sentinel (-1) ETA values,
     * so display_task can render immediately even before
     * eta_fetch_task completes its first fetch cycle — all ETAs
     * will show "--" (sentinel → "--"). */
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

    /* ---- Power management (plan-battery-optimizations.md Phase 1 + 2) ----
     * Configured LAST — after all boot-time hardware init (display SPI,
     * Wi-Fi, RTC, SNTP) has run with PM off, exactly like the pre-optimisation
     * boot.  From here the device spends nearly its whole life in vTaskDelay
     * (fetch wait, render wait, button poll) with Wi-Fi in modem-sleep —
     * ideal for automatic light sleep: whenever both tasks are blocked the
     * SoC sleeps instead of busy-idling.  DFS scales the CPU 160 → 80 MHz
     * when idle; eta_fetch_task holds the CPU at max during the fetch burst
     * so the TLS handshakes stay fast.  Wi-Fi in WIFI_PS_NONE (fetch)
     * automatically blocks light sleep; it re-enters once WIFI_PS_MIN_MODEM
     * is restored. */
    esp_pm_config_t pm_cfg = {
        .max_freq_mhz       = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,  /* 160 */
        .min_freq_mhz       = 80,
        .light_sleep_enable = true,
    };
    esp_err_t pm_err = esp_pm_configure(&pm_cfg);
    if (pm_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure failed (0x%x) — running without light sleep/DFS", pm_err);
    } else {
        ESP_LOGI(TAG, "Power management enabled: light sleep + DFS (idle 80 MHz, burst 160 MHz)");
    }

    esp_err_t lock_err = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "eta_fetch",
                                            &s_fetch_cpu_lock);
    if (lock_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_lock_create failed (0x%x) — fetch burst stays at DFS-idle freq", lock_err);
        s_fetch_cpu_lock = NULL;
    }

    /* Stay awake for the first 30 s after boot so a freshly-booted device
     * can be flashed via the USB-Serial-JTAG auto-reset before light sleep
     * engages (the USJ link drops on every sleep entry, and a light-sleeping
     * device cannot be re-enumerated).  After the grace, the built-in USJ
     * connection monitor (CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION) takes over.
     * Non-fatal if lock/timer creation fails — the device then just needs the
     * manual BOOT+RST download-mode entry to be flashed. */
    lock_err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "usb_boot_grace",
                                  &s_boot_grace_lock);
    if (lock_err == ESP_OK) {
        esp_err_t grace_err = esp_pm_lock_acquire(s_boot_grace_lock);
        if (grace_err != ESP_OK) {
            ESP_LOGW(TAG, "esp_pm_lock_acquire(usb_boot_grace) failed (0x%x)", grace_err);
        } else {
            const esp_timer_create_args_t grace_args = {
                .callback = usb_boot_grace_expired,
                .name = "usb_boot_grace",
            };
            esp_timer_handle_t grace_timer = NULL;
            if (esp_timer_create(&grace_args, &grace_timer) == ESP_OK) {
                esp_timer_start_once(grace_timer, USB_BOOT_GRACE_US);
            }
        }
    } else {
        ESP_LOGW(TAG, "esp_pm_lock_create(usb_boot_grace) failed (0x%x) — no boot grace", lock_err);
        s_boot_grace_lock = NULL;
    }

    /* ---- Create the two FreeRTOS tasks ---- */

    /* display_task: higher priority (tskIDLE_PRIORITY+3) because
     * wall-clock boundary alignment is the most time-critical concern.
     * Stack: 6144 words — U8g2 font rendering + SNTP resync call.
     * The U8g2 page buffer is allocated in the u8g2_st7305 driver
     * (not on this task's stack), so 6144 words is generous for the
     * local variables and call chain. */
    xTaskCreate(display_task, "display_task", 6144,
                NULL, tskIDLE_PRIORITY + 3, NULL);

    /* eta_fetch_task: lower priority (tskIDLE_PRIORITY+2).  Stack:
     * 8192 words — HTTPS/TLS buffers dominate the stack usage here.
     * The HTTP client and TLS handshake internally allocate from
     * heap, but we keep a generous stack to accommodate the
     * fetch_eta() call chain (HTTP request, JSON parsing, etc.). */
    xTaskCreate(eta_fetch_task, "eta_fetch_task", 8192,
                NULL, tskIDLE_PRIORITY + 2, NULL);

    /* app_main has no further work — delete self so the two tasks
     * run independently.  If we returned normally, the idle task
     * would reclaim the main stack, but deleting explicitly is
     * cleaner. */
    vTaskDelete(NULL);
}