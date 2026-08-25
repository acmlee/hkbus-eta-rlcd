#include "http_util.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "http_util";

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
 * In-cycle per-host connection reuse (plan-battery-optimizations.md
 * Phase 3): KMB, Citybus and HKO each get one persistent
 * esp_http_client handle per fetch cycle, so the ~7 back-to-back HTTPS
 * requests collapse to 3 TLS handshakes instead of 7.  Handles are
 * dropped at cycle end via http_close_reuse_clients() — cross-cycle
 * keep-alive is deliberately not relied on (servers close idle
 * connections).  A handle idle longer than HTTP_REUSE_MAX_IDLE_US is
 * dropped proactively so the first request of a cycle never pays a
 * failed-perform + re-init penalty.
 * ----------------------------------------------------------------*/
#define HTTP_MAX_REUSE_HOSTS 3
#define HTTP_REUSE_MAX_IDLE_US (10LL * 1000LL * 1000LL)   /* 10 s */

typedef enum {
    HTTP_REUSE_NONE = -1,
    HTTP_REUSE_KMB,   /* data.etabus.gov.hk */
    HTTP_REUSE_CTB,   /* rt.data.gov.hk */
    HTTP_REUSE_HKO,   /* data.weather.gov.hk */
} http_reuse_host_t;

typedef struct {
    esp_http_client_handle_t handle;
    int64_t last_use_us;
} http_reuse_slot_t;

static http_reuse_slot_t s_reuse_slots[HTTP_MAX_REUSE_HOSTS];

static http_reuse_host_t http_reuse_host_of(const char *url)
{
    if (strstr(url, "data.etabus.gov.hk") != NULL) return HTTP_REUSE_KMB;
    if (strstr(url, "rt.data.gov.hk") != NULL)     return HTTP_REUSE_CTB;
    if (strstr(url, "data.weather.gov.hk") != NULL) return HTTP_REUSE_HKO;
    return HTTP_REUSE_NONE;
}

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
 * Shared request core — performs one GET on an existing client handle
 * and captures the response body.
 *
 * On a transport-level failure (connection unusable) sets *conn_failed
 * (if non-NULL) so the caller knows not to reuse the connection.
 *
 * Returns pointer to malloc'd null-terminated string (caller must
 * free), or NULL on error.
 * ----------------------------------------------------------------*/
static char *http_get_body_common(esp_http_client_handle_t client,
                                  const char *url, const char *log_tag,
                                  bool *conn_failed)
{
    if (client == NULL) return NULL;

    esp_err_t set_err = esp_http_client_set_url(client, url);
    if (set_err != ESP_OK) {
        ESP_LOGW(TAG, "%s set_url failed: %s", log_tag, esp_err_to_name(set_err));
        if (conn_failed) *conn_failed = true;
        return NULL;
    }

    size_t alloc_sz = MAX_BODY_INIT + 1;   /* +1 for null terminator */
    char *buf = malloc(alloc_sz);
    if (buf == NULL) {
        ESP_LOGE(TAG, "malloc %d bytes failed", (int)alloc_sz);
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
        ESP_LOGW(TAG, "%s HTTP error: %s (status=%d)",
                 log_tag, esp_err_to_name(err), status);
        free(buf);
        if (conn_failed) *conn_failed = true;  /* transport dead — not reusable */
        return NULL;
    }

    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "%s HTTP %d", log_tag, status);
        free(buf);
        /* Connection itself may still be healthy — caller decides reuse. */
        return NULL;
    }

    if (capture.error) {
        ESP_LOGW(TAG, "%s body capture error (truncated)", log_tag);
        free(buf);
        return NULL;
    }

    buf[capture.offset] = '\0';

    ESP_LOGI(TAG, "%s → %d bytes (Content-Length=%d), HTTP 200",
             log_tag, (int)capture.offset, content_len);
    return buf;
}

/* ------------------------------------------------------------------
 * One-shot HTTP GET — init, perform, cleanup.  Used for URLs outside
 * the reuse set and as the fallback inside http_get_body_reuse().
 * ----------------------------------------------------------------*/
char *http_get_body(const char *url, const char *log_tag)
{
    esp_http_client_config_t cfg = {
        .url              = url,
        .timeout_ms       = 5000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler    = http_event_handler,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "%s: esp_http_client_init failed", log_tag);
        return NULL;
    }

    char *body = http_get_body_common(client, url, log_tag, NULL);
    esp_http_client_cleanup(client);
    return body;
}

/* ------------------------------------------------------------------
 * Reuse-capable HTTP GET — one persistent handle per host per cycle.
 * ------------------------------------------------------------------*/
char *http_get_body_reuse(const char *url, const char *log_tag)
{
    http_reuse_host_t host = http_reuse_host_of(url);
    if (host == HTTP_REUSE_NONE) {
        /* Not one of the reuse hosts — plain one-shot request. */
        return http_get_body(url, log_tag);
    }

    http_reuse_slot_t *slot = &s_reuse_slots[host];
    int64_t now_us = esp_timer_get_time();

    /* Proactively drop stale connections: servers close idle connections
     * after a few seconds, so a handle idle longer than the window is almost
     * certainly dead.  Dropping it here avoids paying a failed-perform +
     * re-init penalty on every request after the first of a burst. */
    if (slot->handle != NULL &&
        (now_us - slot->last_use_us) > HTTP_REUSE_MAX_IDLE_US) {
        esp_http_client_cleanup(slot->handle);
        slot->handle = NULL;
    }

    bool conn_failed = false;
    char *body = NULL;

    if (slot->handle != NULL) {
        body = http_get_body_common(slot->handle, url, log_tag, &conn_failed);
        if (body != NULL) {
            slot->last_use_us = esp_timer_get_time();
            return body;
        }
    }

    /* First request of the burst, or the existing connection died —
     * drop it and establish a fresh one (retry once). */
    if (conn_failed && slot->handle != NULL) {
        esp_http_client_cleanup(slot->handle);
        slot->handle = NULL;
    }
    if (slot->handle == NULL) {
        esp_http_client_config_t cfg = {
            .url              = url,
            .timeout_ms       = 5000,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .event_handler    = http_event_handler,
        };
        slot->handle = esp_http_client_init(&cfg);
        if (slot->handle == NULL) {
            ESP_LOGE(TAG, "%s: esp_http_client_init failed", log_tag);
            return NULL;
        }
    }

    body = http_get_body_common(slot->handle, url, log_tag, NULL);
    slot->last_use_us = esp_timer_get_time();
    return body;
}

/* ------------------------------------------------------------------
 * Close all per-host reuse handles.  Called at the end of each fetch
 * cycle so stale connections are never reused across cycles.
 * ------------------------------------------------------------------*/
void http_close_reuse_clients(void)
{
    for (int i = 0; i < HTTP_MAX_REUSE_HOSTS; i++) {
        if (s_reuse_slots[i].handle != NULL) {
            esp_http_client_cleanup(s_reuse_slots[i].handle);
            s_reuse_slots[i].handle = NULL;
        }
        s_reuse_slots[i].last_use_us = 0;
    }
}
