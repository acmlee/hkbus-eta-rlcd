#include "weather_hko.h"
#include "http_util.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "weather_hko";

#define HKO_URL "https://data.weather.gov.hk/weatherAPI/opendata/weather.php?dataType=rhrread&lang=en"

/* Stale TTL: 30 minutes */
#define WEATHER_TTL_SEC 1800

/* ---- Internal state (spinlock-protected) ---- */
static weather_temp_t s_weather;
static portMUX_TYPE   s_weather_lock = portMUX_INITIALIZER_UNLOCKED;
static char           s_station[64] = "Hong Kong Observatory";

/* ------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------*/

void weather_init(const char *station_name)
{
    if (station_name && station_name[0] != '\0') {
        strncpy(s_station, station_name, sizeof(s_station) - 1);
        s_station[sizeof(s_station) - 1] = '\0';
    }
    ESP_LOGI(TAG, "weather_init: station='%s'", s_station);
}

void weather_fetch_once(void)
{
    char *body = http_get_body(HKO_URL, "weather");
    if (body == NULL) {
        ESP_LOGW(TAG, "weather_fetch_once: HTTP error");
        return;
    }

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        ESP_LOGW(TAG, "weather_fetch_once: JSON parse failed");
        free(body);
        return;
    }

    /* Navigate: root → "temperature" → "data" (array) */
    cJSON *temp = cJSON_GetObjectItem(root, "temperature");
    if (temp == NULL || !cJSON_IsObject(temp)) {
        ESP_LOGW(TAG, "weather_fetch_once: 'temperature' not found");
        cJSON_Delete(root);
        free(body);
        return;
    }

    cJSON *data = cJSON_GetObjectItem(temp, "data");
    if (data == NULL || !cJSON_IsArray(data)) {
        ESP_LOGW(TAG, "weather_fetch_once: 'temperature.data' is not an array");
        cJSON_Delete(root);
        free(body);
        return;
    }

    int found = 0;
    int array_size = cJSON_GetArraySize(data);
    for (int i = 0; i < array_size; i++) {
        cJSON *entry = cJSON_GetArrayItem(data, i);
        if (entry == NULL) continue;

        cJSON *place = cJSON_GetObjectItem(entry, "place");
        if (place == NULL || !cJSON_IsString(place)) continue;

        if (strcasecmp(place->valuestring, s_station) != 0) continue;

        cJSON *value = cJSON_GetObjectItem(entry, "value");
        if (value == NULL || !cJSON_IsNumber(value)) {
            ESP_LOGW(TAG, "weather_fetch_once: station '%s' has no numeric value",
                     s_station);
            break;
        }

        cJSON *unit = cJSON_GetObjectItem(entry, "unit");
        if (unit == NULL || !cJSON_IsString(unit) ||
            strcmp(unit->valuestring, "C") != 0) {
            ESP_LOGW(TAG, "weather_fetch_once: station '%s' unit is not Celsius",
                     s_station);
            break;
        }

        /* Write under spinlock */
        taskENTER_CRITICAL(&s_weather_lock);
        s_weather.temp_c = (float)value->valuedouble;
        s_weather.last_updated_epoch = time(NULL);
        s_weather.valid = true;
        taskEXIT_CRITICAL(&s_weather_lock);

        ESP_LOGI(TAG, "weather_fetch_once: station='%s' temp=%.1f°C epoch=%ld",
                 s_station, (double)s_weather.temp_c,
                 (long)s_weather.last_updated_epoch);
        found = 1;
        break;
    }

    if (!found) {
        ESP_LOGW(TAG, "weather_fetch_once: station '%s' not found in response",
                 s_station);
    }

    cJSON_Delete(root);
    free(body);
}

bool weather_snapshot(weather_temp_t *out)
{
    if (out == NULL) return false;

    taskENTER_CRITICAL(&s_weather_lock);
    *out = s_weather;
    taskEXIT_CRITICAL(&s_weather_lock);

    if (!out->valid) return false;

    time_t now = time(NULL);
    if (difftime(now, out->last_updated_epoch) > WEATHER_TTL_SEC) {
        return false;  /* stale */
    }

    return true;
}

bool weather_get_temp_str(char *buf, size_t len)
{
    if (buf == NULL || len < 8) return false;

    weather_temp_t snap;
    if (!weather_snapshot(&snap)) {
        return false;
    }

    int temp_int = (int)(snap.temp_c + 0.5f);  /* round to nearest */
    if (temp_int < -99) temp_int = -99;
    if (temp_int > 99)  temp_int = 99;

    snprintf(buf, len, "%d\xC2\xB0""C", temp_int);  /* NN°C */
    return true;
}