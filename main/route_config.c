#include "route_config.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "cJSON.h"
#include <string.h>
#include <stdbool.h>

static const char *TAG = "route_config";

static int refresh_interval = 30;
static char s_weather_station[64] = "Hong Kong Observatory";

/*
 * Normalise operator string from routes.json to internal constant.
 *   "KMB" → "kmb", "CTB" → "citybus"
 * Returns NULL on unrecognised operator.
 */
static const char *normalise_operator(const char *op)
{
    if (strcasecmp(op, "KMB") == 0) return "kmb";
    if (strcasecmp(op, "CTB") == 0) return "citybus";
    return NULL;
}

/* ------------------------------------------------------------------
 * Parse one route object from a cJSON item into a route_config_t.
 * Returns true on success, false if the entry should be skipped
 * (e.g. unknown operator).
 * ----------------------------------------------------------------*/
static bool parse_one_route(cJSON *item, route_config_t *out, int index)
{
    if (item == NULL || out == NULL) return false;

    /* operator */
    cJSON *op = cJSON_GetObjectItem(item, "operator");
    const char *op_str = (op && cJSON_IsString(op)) ? op->valuestring : "";
    const char *norm = normalise_operator(op_str);
    if (norm == NULL) {
        ESP_LOGW(TAG, "route %d: unknown operator '%s', skipping", index, op_str);
        return false;
    }
    strncpy(out->operator, norm, sizeof(out->operator) - 1);
    out->operator[sizeof(out->operator) - 1] = '\0';

    /* route */
    cJSON *r = cJSON_GetObjectItem(item, "route");
    if (r && cJSON_IsString(r)) {
        strncpy(out->route, r->valuestring, sizeof(out->route) - 1);
        out->route[sizeof(out->route) - 1] = '\0';
    }

    /* stop_id */
    cJSON *sid = cJSON_GetObjectItem(item, "stop_id");
    if (sid && cJSON_IsString(sid)) {
        strncpy(out->stop_id, sid->valuestring, sizeof(out->stop_id) - 1);
        out->stop_id[sizeof(out->stop_id) - 1] = '\0';
    }

    /* dest_zh — prefer zh-HK, fall back to dest_en prefixed with "To " */
    cJSON *dz = cJSON_GetObjectItem(item, "dest_zh");
    if (dz && cJSON_IsString(dz) && dz->valuestring[0] != '\0') {
        strncpy(out->dest_zh, dz->valuestring, sizeof(out->dest_zh) - 1);
        out->dest_zh[sizeof(out->dest_zh) - 1] = '\0';
    } else {
        cJSON *de = cJSON_GetObjectItem(item, "dest_en");
        if (de && cJSON_IsString(de) && de->valuestring[0] != '\0') {
            snprintf(out->dest_zh, sizeof(out->dest_zh),
                     "To %s", de->valuestring);
        } else {
            out->dest_zh[0] = '\0';
        }
    }

    /* dest_en — used for direction filtering (KMB terminal stops) */
    cJSON *de = cJSON_GetObjectItem(item, "dest_en");
    if (de && cJSON_IsString(de) && de->valuestring[0] != '\0') {
        strncpy(out->dest_en, de->valuestring, sizeof(out->dest_en) - 1);
        out->dest_en[sizeof(out->dest_en) - 1] = '\0';
    } else {
        out->dest_en[0] = '\0';
    }

    /* stop_zh — prefer zh-HK, fall back to stop_en */
    cJSON *sz = cJSON_GetObjectItem(item, "stop_zh");
    if (sz && cJSON_IsString(sz) && sz->valuestring[0] != '\0') {
        strncpy(out->stop_zh, sz->valuestring, sizeof(out->stop_zh) - 1);
        out->stop_zh[sizeof(out->stop_zh) - 1] = '\0';
    } else {
        cJSON *se = cJSON_GetObjectItem(item, "stop_en");
        if (se && cJSON_IsString(se)) {
            strncpy(out->stop_zh, se->valuestring, sizeof(out->stop_zh) - 1);
            out->stop_zh[sizeof(out->stop_zh) - 1] = '\0';
        } else {
            out->stop_zh[0] = '\0';
        }
    }

    return true;
}

/* ------------------------------------------------------------------
 * Parse a "routes" array (cJSON array) into a page_config_t.
 * Fills up to ROUTES_PER_PAGE entries.
 * ----------------------------------------------------------------*/
static void parse_routes_array(cJSON *routes_arr, page_config_t *page)
{
    page->count = 0;
    if (!cJSON_IsArray(routes_arr)) return;

    int rn = cJSON_GetArraySize(routes_arr);
    if (rn > ROUTES_PER_PAGE) {
        ESP_LOGW(TAG, "%d routes in page, capping to %d", rn, ROUTES_PER_PAGE);
        rn = ROUTES_PER_PAGE;
    }
    for (int i = 0; i < rn; i++) {
        cJSON *item = cJSON_GetArrayItem(routes_arr, i);
        if (parse_one_route(item, &page->routes[page->count], i)) {
            page->count++;
        }
    }
}

/* ------------------------------------------------------------------
 * Load pages from SPIFFS (routes.json) using cJSON.
 * ----------------------------------------------------------------*/
int route_config_load_pages(page_config_t pages[], int max_pages)
{
    /* Mount SPIFFS */
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/spiffs",
        .partition_label        = "storage",
        .max_files              = 4,
        .format_if_mount_failed = false,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return 0;
    }

    /* Read routes.json */
    FILE *f = fopen("/spiffs/routes.json", "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open routes.json");
        esp_vfs_spiffs_unregister(conf.partition_label);
        return 0;
    }

    /* Determine file size */
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (flen <= 0) {
        ESP_LOGE(TAG, "routes.json is empty");
        fclose(f);
        esp_vfs_spiffs_unregister(conf.partition_label);
        return 0;
    }

    char *buf = malloc(flen + 1);
    if (buf == NULL) {
        ESP_LOGE(TAG, "malloc %ld bytes failed", flen + 1);
        fclose(f);
        esp_vfs_spiffs_unregister(conf.partition_label);
        return 0;
    }

    size_t n = fread(buf, 1, flen, f);
    fclose(f);
    buf[n] = '\0';

    ESP_LOGI(TAG, "routes.json (%d bytes)", (int)n);

    /* Parse JSON */
    cJSON *root = cJSON_Parse(buf);
    free(buf);

    if (root == NULL) {
        ESP_LOGE(TAG, "JSON parse failed");
        esp_vfs_spiffs_unregister(conf.partition_label);
        return 0;
    }

    /* Read refresh_seconds (optional, default 30) */
    cJSON *refresh_item = cJSON_GetObjectItem(root, "refresh_seconds");
    if (cJSON_IsNumber(refresh_item)) {
        int val = refresh_item->valueint;
        if (val > 0 && 60 % val == 0) {
            refresh_interval = val;
        } else {
            ESP_LOGW(TAG, "refresh_seconds=%d does not divide 60 evenly, clamping to 10", val);
            refresh_interval = 10;
        }
        ESP_LOGI(TAG, "refresh_interval = %d s", refresh_interval);
    }

    /* Read weather station (optional, default "Hong Kong Observatory") */
    cJSON *weather = cJSON_GetObjectItem(root, "weather");
    if (cJSON_IsObject(weather)) {
        cJSON *station = cJSON_GetObjectItem(weather, "station");
        if (cJSON_IsString(station) && station->valuestring[0] != '\0') {
            strncpy(s_weather_station, station->valuestring, sizeof(s_weather_station) - 1);
            s_weather_station[sizeof(s_weather_station) - 1] = '\0';
        }
    }
    ESP_LOGI(TAG, "weather station = '%s'", s_weather_station);

    /* Read pages array (new format) or legacy routes array (old format) */
    int page_count = 0;
    cJSON *pages_arr = cJSON_GetObjectItem(root, "pages");

    if (cJSON_IsArray(pages_arr)) {
        /* New format: "pages" array */
        int n_arr = cJSON_GetArraySize(pages_arr);
        if (n_arr > max_pages) {
            ESP_LOGW(TAG, "pages array has %d entries, capping to %d", n_arr, max_pages);
            n_arr = max_pages;
        }
        for (int p = 0; p < n_arr; p++) {
            cJSON *page_obj = cJSON_GetArrayItem(pages_arr, p);
            cJSON *routes_arr = cJSON_GetObjectItem(page_obj, "routes");
            if (!cJSON_IsArray(routes_arr)) {
                ESP_LOGW(TAG, "page %d: 'routes' not array, treating as empty", p);
                pages[p].count = 0;
                continue;
            }
            parse_routes_array(routes_arr, &pages[p]);
            page_count++;
        }
    } else {
        /* Legacy: top-level "routes" array → single page */
        cJSON *routes_arr = cJSON_GetObjectItem(root, "routes");
        if (cJSON_IsArray(routes_arr)) {
            parse_routes_array(routes_arr, &pages[0]);
            page_count = 1;
            ESP_LOGI(TAG, "Legacy routes.json format → single page");
        } else {
            ESP_LOGE(TAG, "Neither 'pages' nor 'routes' array found");
        }
    }

    /* Log loaded pages */
    for (int p = 0; p < page_count; p++) {
        ESP_LOGI(TAG, "  page %d: %d routes", p, pages[p].count);
        for (int i = 0; i < pages[p].count; i++) {
            ESP_LOGI(TAG, "    [%d] %s %s → %s (%s)", i,
                     pages[p].routes[i].operator,
                     pages[p].routes[i].route,
                     pages[p].routes[i].dest_zh,
                     pages[p].routes[i].stop_zh);
        }
    }

    cJSON_Delete(root);

    /* Don't unmount — keep SPIFFS mounted for the app lifetime */
    return page_count;
}

int route_config_get_refresh_interval(void)
{
    return refresh_interval;
}

const char *route_config_get_weather_station(void)
{
    return s_weather_station;
}