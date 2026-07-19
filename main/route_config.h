#pragma once

#include "esp_err.h"

#define MAX_ROUTES     16
#define ROUTE_NO_LEN   8
#define STOP_ID_LEN    24
#define DEST_ZH_LEN    64
#define STOP_ZH_LEN    48
#define OPERATOR_LEN   8

/** One monitored route entry loaded from routes.json */
typedef struct {
    char stop_id[STOP_ID_LEN];
    char route[ROUTE_NO_LEN];
    char operator[OPERATOR_LEN];   /** "kmb" or "citybus" */
    char dest_zh[DEST_ZH_LEN];     /** zh-HK destination (e.g. "黃埔花園"); falls back to dest_en */
    char dest_en[DEST_ZH_LEN];     /** en destination (e.g. "CHINA FERRY TERMINAL"); used for direction filtering */
    char stop_zh[STOP_ZH_LEN];     /** zh-HK bus-stop name (e.g. "楊屋道街市"); falls back to stop_en */
} route_config_t;

/**
 * @brief Load route list from SPIFFS (routes.json).
 *
 * Reads the file /spiffs/routes.json and populates config[].
 * Actual cJSON parsing will be refined in a later step.
 *
 * @param config     Output array of route entries
 * @param max_count  Capacity of the array
 * @return Number of routes loaded, or 0 on error.
 */
int route_config_load(route_config_t config[], int max_count);

/**
 * @brief Get the refresh interval in seconds from routes.json.
 * @return Interval in seconds (default 30 on error).
 */
int route_config_get_refresh_interval(void);