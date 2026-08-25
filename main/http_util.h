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

/**
 * @brief HTTP GET with per-host connection reuse within a fetch cycle.
 *
 * Same contract as http_get_body() but reuses a persistent per-host
 * esp_http_client handle for requests made back-to-back in the same
 * cycle (KMB / Citybus / HKO), collapsing repeated TLS handshakes into
 * one per host.  Handles idle longer than a few seconds are dropped
 * proactively, and a dead connection is re-inited once before giving up.
 * Call http_close_reuse_clients() at the end of each fetch cycle.
 *
 * @return Pointer to malloc'd null-terminated string (caller must free),
 *         or NULL on error.
 */
char *http_get_body_reuse(const char *url, const char *log_tag);

/**
 * @brief Close all per-host reuse handles.  Call at the end of each
 *        fetch cycle so stale connections are never reused across cycles.
 */
void http_close_reuse_clients(void);