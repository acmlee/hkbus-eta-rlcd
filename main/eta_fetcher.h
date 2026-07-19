#pragma once

#include "route_config.h"
#include <time.h>

#define KMB_OPERATOR     "kmb"
#define CTB_OPERATOR     "citybus"

#define ETA_ENTRY_DEST_LEN  64
#define ETA_ENTRY_STOP_LEN  32

/** One parsed ETA result from the API */
typedef struct {
    time_t eta_epoch;       /** Raw epoch timestamp; (time_t)-1 = no data / "--" */
    char dest_en[ETA_ENTRY_DEST_LEN];
    char stop_en[ETA_ENTRY_STOP_LEN];
} eta_entry_t;

/**
 * @brief Fetch ETA for one route, dispatch by operator.
 *
 * @param cfg         Route config (operator, stop_id, route number)
 * @param out         Output array of parsed ETA entries
 * @param max_results Capacity of out[] (typically 3 for 3 ETAs)
 * @return Number of entries written to out[], or -1 on HTTP/parse failure
 */
int fetch_eta(const route_config_t *cfg, eta_entry_t *out, int max_results);