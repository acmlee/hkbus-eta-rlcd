#include "http_util.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
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
char *http_get_body(const char *url, const char *log_tag)
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
        ESP_LOGW(TAG, "%s HTTP error: %s (status=%d)",
                 log_tag, esp_err_to_name(err), status);
        free(buf);
        esp_http_client_cleanup(client);
        return NULL;
    }

    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "%s HTTP %d", log_tag, status);
        free(buf);
        esp_http_client_cleanup(client);
        return NULL;
    }

    if (capture.error) {
        ESP_LOGW(TAG, "%s body capture error (truncated)", log_tag);
        free(buf);
        esp_http_client_cleanup(client);
        return NULL;
    }

    buf[capture.offset] = '\0';

    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "%s → %d bytes (Content-Length=%d), HTTP 200",
             log_tag, (int)capture.offset, content_len);
    return buf;
}