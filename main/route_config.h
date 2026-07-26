#pragma once

#include "esp_err.h"

#define MAX_ROUTES     16
#define ROUTE_NO_LEN   8
#define STOP_ID_LEN    24
#define DEST_ZH_LEN    64
#define STOP_ZH_LEN    48
#define OPERATOR_LEN   8

#define MAX_PAGES       2
#define ROUTES_PER_PAGE 3

/** One monitored route entry loaded from routes.json */
typedef struct {
    char stop_id[STOP_ID_LEN];
    char route[ROUTE_NO_LEN];
    char operator[OPERATOR_LEN];   /** "kmb" or "citybus" */
    char dest_zh[DEST_ZH_LEN];     /** zh-HK destination (e.g. "黃埔花園"); falls back to dest_en */
    char dest_en[DEST_ZH_LEN];     /** en destination (e.g. "CHINA FERRY TERMINAL"); used for direction filtering */
    char stop_zh[STOP_ZH_LEN];     /** zh-HK bus-stop name (e.g. "楊屋道街市"); falls back to stop_en */
} route_config_t;

/** One page of up to ROUTES_PER_PAGE routes */
typedef struct {
    route_config_t routes[ROUTES_PER_PAGE];
    int count;                          /** 0..ROUTES_PER_PAGE */
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

/**
 * @brief Get the refresh interval in seconds from routes.json.
 * @return Interval in seconds (default 30 on error).
 */
int route_config_get_refresh_interval(void);

/**
 * @brief Get the configured weather station name from routes.json.
 * @return Pointer to static string, default "Hong Kong Observatory" if absent.
 */
const char *route_config_get_weather_station(void);