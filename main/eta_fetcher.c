#include "eta_fetcher.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include <string.h>
#include <time.h>
#include <stdlib.h>

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
 * Context for HTTP_EVENT_ON_DATA capture — passed as user_data
 * to the event handler.  Buffer grows dynamically via realloc
 * if the response exceeds the initial allocation.
 * ----------------------------------------------------------------*/
#define MAX_BODY_INIT   4096
#define MAX_BODY_CAP    65536

typedef struct {
    char   *buf;
    char  **buf_handle;  /* pointer to caller's buf, for realloc updates */
    size_t  offset;
    size_t  max_size;
    int     error;       /* set to 1 if we overflow or hit a bad event */
} body_capture_t;

/* ------------------------------------------------------------------
 * Event handler — captures response body chunks during perform().
 * Registered per-request, used only for HTTP_EVENT_ON_DATA.
 * ----------------------------------------------------------------*/
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    body_capture_t *cap = (body_capture_t *)evt->user_data;
    if (cap == NULL) return ESP_OK;

    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        size_t avail = cap->max_size - cap->offset;
        size_t to_copy = evt->data_len;
        if (to_copy > avail) {
            size_t need = cap->offset + to_copy;
            if (need > MAX_BODY_CAP) {
                ESP_LOGW(TAG, "body capture overflow: need %d, cap %d",
                         (int)need, MAX_BODY_CAP);
                to_copy = MAX_BODY_CAP - cap->offset;
                cap->error = 1;
            } else {
                size_t new_sz = need + 1024;
                if (new_sz > MAX_BODY_CAP) new_sz = MAX_BODY_CAP;
                char *new_buf = realloc(cap->buf, new_sz + 1);
                if (new_buf == NULL) {
                    ESP_LOGW(TAG, "realloc %d failed", (int)(new_sz + 1));
                    to_copy = avail;
                    cap->error = 1;
                } else {
                    memset(new_buf + cap->max_size + 1, 0,
                           new_sz - cap->max_size);
                    cap->buf = new_buf;
                    *cap->buf_handle = new_buf;
                    cap->max_size = new_sz;
                    avail = cap->max_size - cap->offset;
                    to_copy = evt->data_len;
                }
            }
        }
        memcpy(cap->buf + cap->offset, evt->data, to_copy);
        cap->offset += to_copy;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------
 * Shared HTTP GET — captures response body via event handler
 * during esp_http_client_perform().
 *
 * Returns pointer to malloc'd null-terminated string (caller must
 * free), or NULL on error.
 * ----------------------------------------------------------------*/
static char *http_get_body(const char *url, const char *operator, const char *route)
{
    esp_http_client_config_t cfg = {
        .url              = url,
        .timeout_ms       = 5000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler    = http_event_handler,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);

    /* Pre-allocate a fixed-size buffer for the body.
     * If the response exceeds MAX_BODY_INIT, the event handler
     * will realloc() the buffer dynamically up to MAX_BODY_CAP. */
    size_t alloc_sz = MAX_BODY_INIT + 1;   /* +1 for null terminator */
    char *buf = malloc(alloc_sz);
    if (buf == NULL) {
        ESP_LOGE(TAG, "malloc %d bytes failed", (int)alloc_sz);
        esp_http_client_cleanup(client);
        return NULL;
    }

    body_capture_t capture = {
        .buf        = buf,
        .buf_handle = &buf,
        .offset     = 0,
        .max_size   = MAX_BODY_INIT,
        .error      = 0,
    };

    esp_http_client_set_user_data(client, &capture);

    esp_err_t err = esp_http_client_perform(client);

    /* Capture the Content-Length regardless of success/failure for logging */
    int content_len = esp_http_client_get_content_length(client);

    if (err != ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGW(TAG, "%s route %s HTTP error: %s (status=%d)",
                 operator, route, esp_err_to_name(err), status);
        free(buf);
        esp_http_client_cleanup(client);
        return NULL;
    }

    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "%s route %s HTTP %d", operator, route, status);
        free(buf);
        esp_http_client_cleanup(client);
        return NULL;
    }

    if (capture.error) {
        ESP_LOGW(TAG, "%s route %s body capture error (truncated)",
                 operator, route);
        free(buf);
        esp_http_client_cleanup(client);
        return NULL;
    }

    buf[capture.offset] = '\0';

    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "%s route %s → %d bytes (Content-Length=%d), HTTP 200",
             operator, route, (int)capture.offset, content_len);
    return buf;
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

        char *body = http_get_body(url, "kmb", cfg->route);
        if (body == NULL) return -1;

        int ret = parse_kmb_response(body, cfg, out, max_results);
        free(body);
        return ret;

    } else if (strcmp(cfg->operator, CTB_OPERATOR) == 0) {
        char url[192];
        snprintf(url, sizeof(url),
                 "https://rt.data.gov.hk/v2/transport/citybus/eta/CTB/%s/%s",
                 cfg->stop_id, cfg->route);

        char *body = http_get_body(url, "citybus", cfg->route);
        if (body == NULL) return -1;

        int ret = parse_citybus_response(body, cfg, out, max_results);
        free(body);
        return ret;

    } else {
        ESP_LOGW(TAG, "Unknown operator: %s", cfg->operator);
        return -1;
    }
}