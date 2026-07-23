#include "eta_fetcher.h"
#include "http_util.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "eta_fetcher";

/* ------------------------------------------------------------------
 * ISO 8601 parser: "2026-07-06T14:32:00+08:00" → raw epoch time_t.
 * Returns (time_t)-1 on parse failure.
 * ----------------------------------------------------------------*/
static time_t parse_eta_epoch(const char *eta_str)
{
    struct tm tm = {0};
    int tz_h = 0, tz_m = 0;
    int parsed = sscanf(eta_str,
                        "%d-%d-%dT%d:%d:%d%*c%02d:%02d",
                        &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                        &tm.tm_hour, &tm.tm_min, &tm.tm_sec,
                        &tz_h, &tz_m);
    if (parsed < 6) return (time_t)-1;
    if (parsed < 8) {
        /* No timezone parsed — assume +08:00 (HKT) */
        tz_h = 8;
        tz_m = 0;
    }

    tm.tm_year -= 1900;
    tm.tm_mon  -= 1;
    tm.tm_isdst = 0;

    time_t eta_ts = mktime(&tm);
    if (eta_ts == (time_t)-1) return (time_t)-1;

    /* Adjust for the timestamp's timezone offset to HKT */
    int tz_offset_min = tz_h * 60 + (tz_h >= 0 ? tz_m : -tz_m);
    int hkt_offset = 8 * 60;
    int diff = tz_offset_min - hkt_offset;
    eta_ts -= diff * 60;

    return eta_ts;
}

/* ------------------------------------------------------------------
 * Comparison helper for sorting ETAs by eta_epoch (earliest first).
 * ----------------------------------------------------------------*/
static int cmp_eta(const void *a, const void *b)
{
    const eta_entry_t *ea = (const eta_entry_t *)a;
    const eta_entry_t *eb = (const eta_entry_t *)b;
    if (ea->eta_epoch < eb->eta_epoch) return -1;
    if (ea->eta_epoch > eb->eta_epoch) return 1;
    return 0;
}

/* ------------------------------------------------------------------
 * Extract up to max_results ETAs from a KMB API response.
 *
 * KMB route-specific endpoint returns only the requested route.
 * We take all entries, sort by eta_seq (earliest first), and write
 * up to max_results.
 *
 * Returns count written, or -1 on failure.
 * ----------------------------------------------------------------*/
static int parse_kmb_response(const char *json_body, const route_config_t *cfg,
                              eta_entry_t *out, int max_results)
{
    cJSON *root = cJSON_Parse(json_body);
    if (root == NULL) {
        ESP_LOGW(TAG, "kmb %s: JSON parse failed", cfg->route);
        return -1;
    }

    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (data == NULL || !cJSON_IsArray(data)) {
        ESP_LOGW(TAG, "kmb %s: 'data' is not an array", cfg->route);
        cJSON_Delete(root);
        return -1;
    }

    int array_size = cJSON_GetArraySize(data);
    if (array_size == 0) {
        cJSON_Delete(root);
        return 0;
    }

    int collected = 0;
    size_t alloc_sz = array_size * sizeof(eta_entry_t);
    eta_entry_t *tmp = malloc(alloc_sz);
    if (tmp == NULL) {
        cJSON_Delete(root);
        return -1;
    }

    for (int i = 0; i < array_size; i++) {
        cJSON *item = cJSON_GetArrayItem(data, i);
        if (item == NULL) {
            ESP_LOGD(TAG, "kmb %s entry[%d]: item is NULL — skipped", cfg->route, i);
            continue;
        }

        /* Get the raw dest_en from the API for diagnostic logging */
        cJSON *api_dest = cJSON_GetObjectItem(item, "dest_en");
        const char *api_dest_str = (api_dest && cJSON_IsString(api_dest))
                                   ? api_dest->valuestring : "(absent)";

        /* Parse eta timestamp */
        cJSON *eta_json = cJSON_GetObjectItem(item, "eta");
        if (eta_json == NULL || !cJSON_IsString(eta_json)) {
            ESP_LOGD(TAG, "kmb %s entry[%d]: eta= (absent/null) api_dest=\"%s\" "
                     "cfg_dest=\"%s\" — skipped (missing eta)",
                     cfg->route, i, api_dest_str, cfg->dest_en);
            continue;
        }

        /* Direction filter: skip entries whose dest_en doesn't match the
         * configured route dest_en. This handles terminal stops where the
         * API returns both outbound and inbound directions.
         * Skip filtering if cfg->dest_en is empty (fallback: show all). */
        if (cfg->dest_en[0] != '\0') {
            if (api_dest && cJSON_IsString(api_dest) &&
                strcasecmp(api_dest->valuestring, cfg->dest_en) != 0) {
                ESP_LOGD(TAG, "kmb %s entry[%d]: eta=\"%s\" api_dest=\"%s\" "
                         "cfg_dest=\"%s\" — skipped (direction)",
                         cfg->route, i, eta_json->valuestring,
                         api_dest_str, cfg->dest_en);
                continue;  /* wrong direction, skip */
            }
        }

        time_t epoch = parse_eta_epoch(eta_json->valuestring);
        if (epoch == (time_t)-1) {
            ESP_LOGD(TAG, "kmb %s entry[%d]: eta=\"%s\" api_dest=\"%s\" "
                     "cfg_dest=\"%s\" — skipped (parse)",
                     cfg->route, i, eta_json->valuestring,
                     api_dest_str, cfg->dest_en);
            continue;
        }

        tmp[collected].eta_epoch = epoch;

        cJSON *dest = cJSON_GetObjectItem(item, "dest_en");
        if (dest && cJSON_IsString(dest)) {
            strncpy(tmp[collected].dest_en, dest->valuestring,
                    sizeof(tmp[collected].dest_en) - 1);
            tmp[collected].dest_en[sizeof(tmp[collected].dest_en) - 1] = '\0';
        } else {
            tmp[collected].dest_en[0] = '\0';
        }

        /* KMB response does not typically include stop_en per entry */
        tmp[collected].stop_en[0] = '\0';

        ESP_LOGD(TAG, "kmb %s entry[%d]: eta=\"%s\" api_dest=\"%s\" "
                 "cfg_dest=\"%s\" epoch=%ld — accepted",
                 cfg->route, i, eta_json->valuestring,
                 api_dest_str, cfg->dest_en, (long)epoch);
        collected++;
    }

    cJSON_Delete(root);

    if (collected == 0 && cfg->dest_en[0] != '\0') {
        ESP_LOGW(TAG, "kmb %s: all %d entries skipped (no usable ETAs) — "
                 "dest_en='%s'", cfg->route, array_size, cfg->dest_en);
    }

    if (collected == 0) {
        free(tmp);
        return 0;
    }

    /* Sort by eta_epoch (earliest first) */
    qsort(tmp, collected, sizeof(eta_entry_t), cmp_eta);

    /* Write up to max_results */
    int count = (collected < max_results) ? collected : max_results;
    memcpy(out, tmp, count * sizeof(eta_entry_t));

    free(tmp);
    return count;
}

/* ------------------------------------------------------------------
 * Extract up to max_results ETAs from a Citybus API response.
 *
 * Citybus returns ONLY the requested route. We take the first
 * max_results entries with valid timestamps.
 *
 * Returns count written, or -1 on failure.
 * ----------------------------------------------------------------*/
static int parse_citybus_response(const char *json_body, const route_config_t *cfg,
                                  eta_entry_t *out, int max_results)
{
    cJSON *root = cJSON_Parse(json_body);
    if (root == NULL) {
        ESP_LOGW(TAG, "citybus %s: JSON parse failed", cfg->route);
        return -1;
    }

    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (data == NULL || !cJSON_IsArray(data)) {
        ESP_LOGW(TAG, "citybus %s: 'data' is not an array", cfg->route);
        cJSON_Delete(root);
        return -1;
    }

    int array_size = cJSON_GetArraySize(data);
    if (array_size == 0) {
        cJSON_Delete(root);
        return 0;
    }

    int count = 0;
    for (int i = 0; i < array_size && count < max_results; i++) {
        cJSON *item = cJSON_GetArrayItem(data, i);
        if (item == NULL) continue;

        cJSON *eta_json = cJSON_GetObjectItem(item, "eta");
        if (eta_json == NULL || !cJSON_IsString(eta_json)) continue;

        time_t epoch = parse_eta_epoch(eta_json->valuestring);
        if (epoch == (time_t)-1) continue;

        out[count].eta_epoch = epoch;

        cJSON *dest = cJSON_GetObjectItem(item, "dest_en");
        if (dest && cJSON_IsString(dest)) {
            strncpy(out[count].dest_en, dest->valuestring,
                    sizeof(out[count].dest_en) - 1);
            out[count].dest_en[sizeof(out[count].dest_en) - 1] = '\0';
        } else {
            out[count].dest_en[0] = '\0';
        }

        out[count].stop_en[0] = '\0';
        count++;
    }

    cJSON_Delete(root);
    return count;
}

/* ------------------------------------------------------------------
 * fetch_eta() — Common entry point. Dispatches internally based on
 * cfg->operator ("kmb" or "citybus").
 *
 * Returns number of entries written to out[], or -1 on HTTP/parse
 * failure (whole-request failure, not per-entry).
 * ----------------------------------------------------------------*/
int fetch_eta(const route_config_t *cfg, eta_entry_t *out, int max_results)
{
    if (cfg == NULL || out == NULL || max_results <= 0) return -1;

    if (strcmp(cfg->operator, KMB_OPERATOR) == 0) {
        char url[256];
        snprintf(url, sizeof(url),
                 "https://data.etabus.gov.hk/v1/transport/kmb/eta/%s/%s/1",
                 cfg->stop_id, cfg->route);

        char log_buf[48];
        snprintf(log_buf, sizeof(log_buf), "eta kmb %s", cfg->route);
        char *body = http_get_body(url, log_buf);
        if (body == NULL) return -1;

        int ret = parse_kmb_response(body, cfg, out, max_results);
        free(body);
        return ret;

    } else if (strcmp(cfg->operator, CTB_OPERATOR) == 0) {
        char url[192];
        snprintf(url, sizeof(url),
                 "https://rt.data.gov.hk/v2/transport/citybus/eta/CTB/%s/%s",
                 cfg->stop_id, cfg->route);

        char log_buf[48];
        snprintf(log_buf, sizeof(log_buf), "eta ctb %s", cfg->route);
        char *body = http_get_body(url, log_buf);
        if (body == NULL) return -1;

        int ret = parse_citybus_response(body, cfg, out, max_results);
        free(body);
        return ret;

    } else {
        ESP_LOGW(TAG, "Unknown operator: %s", cfg->operator);
        return -1;
    }
}