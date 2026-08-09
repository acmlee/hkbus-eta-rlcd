#pragma once

#include <stdbool.h>
#include <time.h>
#include <stddef.h>

/** Shared weather state — spinlock-protected (writer: eta_fetch_task,
 *  reader: display_task). Both fields are written by the same fetch,
 *  so they share one last_updated_epoch / TTL. */
typedef struct {
    float  temp_c;             /** Temperature in Celsius */
    int    humidity_pct;       /** Relative humidity in percent (0–100) */
    time_t last_updated_epoch; /** Epoch of last successful fetch */
    bool   valid;              /** True if a fetch has ever succeeded */
    bool   humidity_valid;     /** True if the last successful fetch found
                                    a humidity entry for the configured station */
} weather_t;

/**
 * @brief Initialise the weather module with the configured station name.
 *
 * Copies station_name into an internal buffer.  Must be called once
 * during boot, after route_config_load().
 */
void weather_init(const char *station_name);

/**
 * @brief Fetch temperature from HKO rhrread API, parse JSON, update
 *        internal spinlock-protected state.
 *
 * Intended to be called from eta_fetch_task every 20th cycle (~10 min).
 * On failure, last-known-good data is preserved (no modification).
 */
void weather_fetch_once(void);

/**
 * @brief Snapshot the current weather state under spinlock.
 *
 * @param out  Pointer to weather_t to fill
 * @return false if data is invalid or stale (>30 min TTL), true otherwise
 */
bool weather_snapshot(weather_t *out);

/**
 * @brief Convenience: fill a pre-formatted "NN°C" string.
 *
 * @param buf  Output buffer (recommended size 8 bytes minimum)
 * @param len  Buffer size
 * @return false if unavailable/stale, true if buffer was filled
 */
bool weather_get_temp_str(char *buf, size_t len);

/**
 * @brief Convenience: fill a pre-formatted "NN%" humidity string.
 *
 * @param buf  Output buffer (recommended size 8 bytes minimum)
 * @param len  Buffer size
 * @return false if unavailable/stale, true if buffer was filled
 */
bool weather_get_humidity_str(char *buf, size_t len);