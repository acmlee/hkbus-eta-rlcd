#pragma once

#include <stddef.h>

/**
 * @brief HTTP GET the response body from a URL.
 *
 * Uses esp_http_client with the ESP certificate bundle, 5s timeout,
 * and dynamic buffer growth up to MAX_BODY_CAP (65536 bytes).
 *
 * @param url     Full URL to GET
 * @param log_tag Tag string for log messages (e.g. "eta kmb 30X" or "weather")
 * @return Pointer to malloc'd null-terminated string (caller must free),
 *         or NULL on error.
 */
char *http_get_body(const char *url, const char *log_tag);