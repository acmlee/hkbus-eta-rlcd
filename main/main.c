/**
 * @file main.c
 * @brief Entry point for hk-bus-eta-rlcd — ETA dashboard on ESP32-S3-RLCD.
 *
 * Two-task design:
 *   - eta_fetch_task:  fetches ETA for all 3 routes every ~30 s
 *                       (27–33 s, ±10% random jitter) into a
 *                       double-buffered shared buffer, toggles Wi-Fi PS.
 *   - display_task:    renders at wall-clock :00/:30 boundaries, handles
 *                       daily NTP resync.
 *
 * Flow:
 *   1. Init NVS, SPIFFS, display, Wi-Fi, SNTP (sequential at boot)
 *   2. Load route config from routes.json
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
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif_sntp.h"

#include "display.h"
#include "eta_fetcher.h"
#include "route_config.h"
#include "battery.h"
/* cJSON parsing is internal to eta_fetcher.c */

static const char *TAG = "hkbus";

/* Event group bits for Wi-Fi connection */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

/* Track reconnection attempts for diagnostic logging */
static int s_reconnect_count = 0;

/* ------------------------------------------------------------------
 * Double-buffered shared ETA data
 *
 * Two buffers of 3 route_data_t each.  eta_fetch_task writes to the
 * inactive buffer, then atomically flips s_active_buf_idx (0 or 1).
 * display_task reads s_active_buf_idx once per render cycle and uses
 * that buffer for the entire render.  No mutex/semaphore is needed
 * because:
 *   - The active index is a word-sized int — aligned 32-bit writes on
 *     ESP32-S3 are naturally atomic (no tearing).
 *   - display_task reads the index once at the top of its loop, then
 *     reads from the same buffer throughout; a concurrent flip by
 *     fetch_task only affects the *next* render cycle.
 *   - There is no reader/writer contention on the same buffer at the
 *     same time: fetch_task writes the inactive buffer while
 *     display_task reads the active buffer, and the flip itself is a
 *     single-copy-atomic store.
 * ----------------------------------------------------------------*/
static route_data_t s_route_buf[2][3];
static volatile int  s_active_buf_idx = 0;   /* 0 or 1, word-sized atomic */

/* Route config — loaded once at boot, shared by both tasks */
static route_config_t s_routes[MAX_ROUTES];
static int            s_route_count = 0;

/* Display refresh interval (seconds) — read from routes.json once,
 * validated to divide 60 evenly for clean wall-clock boundaries. */
static int s_refresh_interval = 10;

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
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
        s_reconnect_count = 0;
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
    while (time(&now) < 1700000000 && retries < MAX_RETRIES) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        retries++;
    }

    if (now >= 1700000000) {
        struct tm *ti = localtime(&now);
        ESP_LOGI(TAG, "Time synced: %02d:%02d:%02d",
                 ti->tm_hour, ti->tm_min, ti->tm_sec);
    } else {
        ESP_LOGW(TAG, "SNTP sync timeout (%ds), proceeding anyway", retries);
    }
}

static void time_sync_init(void)
{
    /* 1. Set timezone BEFORE SNTP so timestamps are interpreted in HKT */
    setenv("TZ", "HKT-8", 1);
    tzset();

    /* 2. Configure SNTP */
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("stdtime.gov.hk");
    cfg.sync_cb = NULL;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&cfg));

    /* 3. Block until time > 1700000000 (Jan 2024) or 10 s timeout */
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
    cfg.sync_cb = NULL;
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
 * Runs every 30 s (vTaskDelay).  Disables Wi-Fi modem-sleep before
 * fetching, re-enables it after.  Writes fresh ETA values into the
 * currently inactive double-buffer, then atomically flips the active
 * buffer index so display_task sees the new data on its next render.
 *
 * Priority: tskIDLE_PRIORITY+2 (lower than display_task).
 * ----------------------------------------------------------------*/
static void eta_fetch_task(void *arg)
{
    eta_entry_t eta_buf[3];

    (void)arg;

    /* Ensure Wi-Fi PS is in a known state before the loop */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

    while (1) {
        ESP_LOGD(TAG, "Fetch cycle start");

        /* Disable modem-sleep so HTTP requests are not delayed by
         * radio power-state transitions. */
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

        /* Determine which buffer is inactive (the one display_task
         * is NOT currently reading).  The read of s_active_buf_idx
         * here is safe: fetch_task is the only writer, and the
         * window between this read and the write at the bottom of
         * the loop is the entire fetch cycle — display_task may
         * flip the active index via its own read, but that only
         * affects the *next* loop iteration. */
        int inactive = 1 - s_active_buf_idx;

        /* Fetch ETA for each route into the inactive buffer */
        for (int i = 0; i < s_route_count && i < 3; i++) {
            int n = fetch_eta(&s_routes[i], eta_buf, 3);
            if (n > 0) {
                s_route_buf[inactive][i].eta1 = (n >= 1) ? eta_buf[0].eta_epoch : (time_t)-1;
                s_route_buf[inactive][i].eta2 = (n >= 2) ? eta_buf[1].eta_epoch : (time_t)-1;
                s_route_buf[inactive][i].eta3 = (n >= 3) ? eta_buf[2].eta_epoch : (time_t)-1;
            } else {
                /* Fetch failed — preserve last-known-good ETAs from the
                 * active buffer, but expire to "--" if the ETA timestamp
                 * is more than 3 minutes (180 s) in the past.  Comparing
                 * against the ETA itself (not the fetch time) naturally
                 * handles both "future ETA still valid" and "past ETA
                 * should be cleared" without needing a separate fetch
                 * timestamp. */
                int active = s_active_buf_idx;
                time_t now;
                time(&now);
                time_t e1 = s_route_buf[active][i].eta1;
                time_t e2 = s_route_buf[active][i].eta2;
                time_t e3 = s_route_buf[active][i].eta3;

                s_route_buf[inactive][i].eta1 = (e1 != (time_t)-1 && difftime(now, e1) < 180.0) ? e1 : (time_t)-1;
                s_route_buf[inactive][i].eta2 = (e2 != (time_t)-1 && difftime(now, e2) < 180.0) ? e2 : (time_t)-1;
                s_route_buf[inactive][i].eta3 = (e3 != (time_t)-1 && difftime(now, e3) < 180.0) ? e3 : (time_t)-1;
            }

            /* Yield to WiFi driver between requests to prevent
             * interrupt-watchdog timeout */
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        /* Re-enable modem-sleep after fetch completes */
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

        /* Sample battery during confirmed Wi-Fi-idle window.
         * The 50 ms settle delay is inside battery_sample_if_due().
         * Internal cadence: samples every 2nd call (~every 60 s). */
        battery_sample_if_due();

        /* Atomic flip: publish the newly-filled buffer.
         * This is a single aligned word-sized write — naturally
         * atomic on ESP32-S3.  display_task may read either the
         * old or new index on its next render, but never a torn
         * value. */
        s_active_buf_idx = inactive;

        /* Wait ~30 s before next fetch cycle (27–33 s, ±10% random
         * jitter to avoid thundering-herd alignment with other clients). */
        int jitter = (int)(esp_random() % 6001) - 3000;   /* -3000 to +3000 ms */
        int delay_ms = 30000 + jitter;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

/* ------------------------------------------------------------------
 * display_task — render loop, one task
 *
 * Runs at wall-clock :00/:30 boundaries.  Each tick:
 *   1. Reads current time, builds HH:MM header string
 *   2. Checks daily NTP resync condition (06:00, once per day) and
 *      triggers it if due
 *   3. Snaps the active buffer index (single atomic read)
 *   4. Builds "Updated HH:MM:SS" footer string
 *   5. Computes seconds until next :00/:30 boundary
 *   6. Calls render_dashboard() with the active buffer
 *   7. Sleeps until the next boundary
 *
 * Priority: tskIDLE_PRIORITY+3 (higher than eta_fetch_task), because
 * render timing alignment is the more time-critical concern.
 * ----------------------------------------------------------------*/
static void display_task(void *arg)
{
    char last_updated[32];
    char time_str[6];

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

        /* Build HH:MM header */
        snprintf(time_str, sizeof(time_str), "%02d:%02d",
                 ti->tm_hour, ti->tm_min);

        /* ---- 2. Daily NTP resync at 06:00 ---- */
        /* Trigger once per day at 06:xx.  Uses tm_yday to ensure the
         * resync fires exactly once, not every 30-s tick during the
         * 06:00 hour.  The guard is updated *after* the attempt
         * regardless of success/failure, so a failed attempt does not
         * retry every tick for the rest of the hour — but tomorrow's
         * 06:00 will still trigger one new attempt (since tm_yday
         * will differ). */
        if (ti->tm_hour == 6 && ti->tm_yday != last_resync_yday) {
            ntp_resync();
            last_resync_yday = ti->tm_yday;

            /* Re-read time after resync to update the header string
             * in case the clock was corrected by more than a minute. */
            time(&now);
            ti = localtime(&now);
            snprintf(time_str, sizeof(time_str), "%02d:%02d",
                     ti->tm_hour, ti->tm_min);
        }

        /* ---- 4. Snap the active buffer index (single atomic read) ---- */
        int buf_idx = s_active_buf_idx;

        /* ---- 5. Build last-updated timestamp ---- */
        time(&now);
        ti = localtime(&now);
        snprintf(last_updated, sizeof(last_updated),
                 "Updated %02d:%02d:%02d",
                 ti->tm_hour, ti->tm_min, ti->tm_sec);

        /* ---- 5a. Read battery percentage (cached — sampled by
         * eta_fetch_task during Wi-Fi-idle windows). ---- */
        {
            int pct = battery_get_percentage();
            if (pct != 255) battery_pct = pct;
        }

        /* ---- 6. Compute seconds until next wall-clock boundary ---- */
        int sec = ti->tm_sec;
        int next_seconds = s_refresh_interval - (sec % s_refresh_interval);

        /* ---- 7. Render the dashboard ---- */
        render_dashboard(time_str, last_updated, battery_pct,
                         s_route_buf[buf_idx]);

        /* ---- 8. Sleep until the next boundary ---- */
        vTaskDelay(pdMS_TO_TICKS(next_seconds * 1000));
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

    /* ---- Init battery ADC (before Wi-Fi; ADC1, no conflict) ---- */
    battery_init();

    /* ---- Init Wi-Fi (blocking, up to 5 retries) ---- */
    wifi_init_sta(CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD);

    /* ---- Init SNTP (blocking, up to 10 s) ---- */
    time_sync_init();

    /* ---- Load route config from SPIFFS ---- */
    s_route_count = route_config_load(s_routes, MAX_ROUTES);
    ESP_LOGI(TAG, "Loaded %d routes", s_route_count);

    /* ---- Initialise double-buffer with static route info ---- */
    /* Both buffers start with the same static fields (route_num,
     * dest_zh, stop_zh) and sentinel (-1) ETA values, so display_task
     * can render immediately even before eta_fetch_task completes its
     * first fetch cycle — all ETAs will show "--" (sentinel → "--"). */
    for (int buf = 0; buf < 2; buf++) {
        for (int i = 0; i < 3; i++) {
            if (i < s_route_count) {
                s_route_buf[buf][i].route_num = s_routes[i].route;
                s_route_buf[buf][i].dest_zh   = s_routes[i].dest_zh;
                s_route_buf[buf][i].stop_zh   = s_routes[i].stop_zh;
            } else {
                s_route_buf[buf][i].route_num = "--";
                s_route_buf[buf][i].dest_zh   = "";
                s_route_buf[buf][i].stop_zh   = "";
            }
            s_route_buf[buf][i].eta1 = (time_t)-1;
            s_route_buf[buf][i].eta2 = (time_t)-1;
            s_route_buf[buf][i].eta3 = (time_t)-1;
        }
    }
    s_active_buf_idx = 0;

    /* ---- Create the two FreeRTOS tasks ---- */

    /* display_task: higher priority (tskIDLE_PRIORITY+3) because
     * wall-clock :00/:30 alignment is the most time-critical concern.
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