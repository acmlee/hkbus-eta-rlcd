#include "route_config.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "cJSON.h"
#include <string.h>

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
 * Load route list from SPIFFS (routes.json) using cJSON.
 * ----------------------------------------------------------------*/
int route_config_load(route_config_t config[], int max_count)
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
    /* Must divide 60 evenly for clean wall-clock boundary alignment. */
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

    /* Read routes array */
    cJSON *routes = cJSON_GetObjectItem(root, "routes");
    if (routes == NULL || !cJSON_IsArray(routes)) {
        ESP_LOGE(TAG, "'routes' is not an array");
        cJSON_Delete(root);
        esp_vfs_spiffs_unregister(conf.partition_label);
        return 0;
    }

    int count = cJSON_GetArraySize(routes);
    if (count > max_count) count = max_count;

    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(routes, i);
        if (item == NULL) continue;

        /* operator */
        cJSON *op = cJSON_GetObjectItem(item, "operator");
        const char *op_str = (op && cJSON_IsString(op)) ? op->valuestring : "";
        const char *norm = normalise_operator(op_str);
        if (norm == NULL) {
            ESP_LOGW(TAG, "route %d: unknown operator '%s', skipping", i, op_str);
            continue;
        }
        strncpy(config[i].operator, norm, sizeof(config[i].operator) - 1);
        config[i].operator[sizeof(config[i].operator) - 1] = '\0';

        /* route */
        cJSON *r = cJSON_GetObjectItem(item, "route");
        if (r && cJSON_IsString(r)) {
            strncpy(config[i].route, r->valuestring, sizeof(config[i].route) - 1);
            config[i].route[sizeof(config[i].route) - 1] = '\0';
        }

        /* stop_id */
        cJSON *sid = cJSON_GetObjectItem(item, "stop_id");
        if (sid && cJSON_IsString(sid)) {
            strncpy(config[i].stop_id, sid->valuestring, sizeof(config[i].stop_id) - 1);
            config[i].stop_id[sizeof(config[i].stop_id) - 1] = '\0';
        }

        /* dest_zh — prefer zh-HK, fall back to dest_en prefixed with "To " */
        cJSON *dz = cJSON_GetObjectItem(item, "dest_zh");
        if (dz && cJSON_IsString(dz) && dz->valuestring[0] != '\0') {
            strncpy(config[i].dest_zh, dz->valuestring, sizeof(config[i].dest_zh) - 1);
            config[i].dest_zh[sizeof(config[i].dest_zh) - 1] = '\0';
        } else {
            cJSON *de = cJSON_GetObjectItem(item, "dest_en");
            if (de && cJSON_IsString(de) && de->valuestring[0] != '\0') {
                snprintf(config[i].dest_zh, sizeof(config[i].dest_zh),
                         "To %s", de->valuestring);
            } else {
                config[i].dest_zh[0] = '\0';
            }
        }

        /* dest_en — used for direction filtering (KMB terminal stops) */
        cJSON *de = cJSON_GetObjectItem(item, "dest_en");
        if (de && cJSON_IsString(de) && de->valuestring[0] != '\0') {
            strncpy(config[i].dest_en, de->valuestring, sizeof(config[i].dest_en) - 1);
            config[i].dest_en[sizeof(config[i].dest_en) - 1] = '\0';
        } else {
            config[i].dest_en[0] = '\0';
        }

        /* stop_zh — prefer zh-HK, fall back to stop_en */
        cJSON *sz = cJSON_GetObjectItem(item, "stop_zh");
        if (sz && cJSON_IsString(sz) && sz->valuestring[0] != '\0') {
            strncpy(config[i].stop_zh, sz->valuestring, sizeof(config[i].stop_zh) - 1);
            config[i].stop_zh[sizeof(config[i].stop_zh) - 1] = '\0';
        } else {
            cJSON *se = cJSON_GetObjectItem(item, "stop_en");
            if (se && cJSON_IsString(se)) {
                strncpy(config[i].stop_zh, se->valuestring, sizeof(config[i].stop_zh) - 1);
                config[i].stop_zh[sizeof(config[i].stop_zh) - 1] = '\0';
            } else {
                config[i].stop_zh[0] = '\0';
            }
        }

        ESP_LOGI(TAG, "  [%d] %s %s → %s (%s)", i,
                 config[i].operator, config[i].route,
                 config[i].dest_zh, config[i].stop_zh);
    }

    cJSON_Delete(root);

    /* Don't unmount — keep SPIFFS mounted for the app lifetime */
    return count;
}

int route_config_get_refresh_interval(void)
{
    return refresh_interval;
}

const char *route_config_get_weather_station(void)
{
    return s_weather_station;
}