#include "battery.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "battery";

static adc_oneshot_unit_handle_t adc1_handle = NULL;
static adc_cali_handle_t cali_handle = NULL;
static bool cali_enabled = false;

/* ------------------------------------------------------------------
 * Piecewise linear Li-ion discharge curve lookup table
 *
 * Approximates the real non-linear discharge curve of a typical
 * 18650 Li-ion cell.  The voltage plateau in the 3.7-3.8V range
 * corresponds to ~30-50% capacity, while the steep drop below 3.5V
 * and above 4.0V is captured by the tighter point spacing at the
 * ends.
 * ----------------------------------------------------------------*/
typedef struct {
    float voltage;
    uint8_t percent;
} battery_curve_point_t;

static const battery_curve_point_t battery_curve[] = {
    {4.20f, 100},
    {4.10f,  95},
    {4.00f,  85},
    {3.90f,  70},
    {3.80f,  50},
    {3.70f,  30},
    {3.60f,  15},
    {3.50f,   8},
    {3.40f,   3},
    {3.30f,   1},
    {3.00f,   0},
};

/**
 * @brief Convert battery voltage (V) to percentage using
 *        piecewise linear interpolation of the lookup table.
 *        Clamps to 0-100.
 */
static uint8_t battery_voltage_to_percent(float v)
{
    const int n = sizeof(battery_curve) / sizeof(battery_curve[0]);

    if (v >= battery_curve[0].voltage) return 100;
    if (v <= battery_curve[n - 1].voltage) return 0;

    for (int i = 0; i < n - 1; i++) {
        float v_hi = battery_curve[i].voltage;
        float v_lo = battery_curve[i + 1].voltage;
        if (v <= v_hi && v >= v_lo) {
            float frac = (v - v_lo) / (v_hi - v_lo);
            float pct = (float)battery_curve[i + 1].percent
                        + frac * (float)(battery_curve[i].percent - battery_curve[i + 1].percent);
            return (uint8_t)(pct + 0.5f);
        }
    }
    return 0;
}

/* ==================================================================
 * Filtering pipeline — all static/internal to battery.c
 *
 * The pipeline is:
 *   1. Raw ADC read (16-sample average, same as before)
 *   2. Push into rolling history buffer (5 slots)
 *   3. Compute median of history
 *   4. Apply EMA to median
 *   5. Map to percentage via LUT
 *   6. Two-reading confirmation for large jumps (>6 points)
 *   7. Store accepted value for display
 *
 * A spinlock protects all shared state since battery_sample_if_due()
 * runs in eta_fetch_task and battery_get_percentage() runs in
 * display_task.
 * ================================================================*/

/* Rolling history buffer — 5 raw voltage readings (V) */
#define HISTORY_SIZE 5
static float s_history[HISTORY_SIZE];
static int   s_hist_count = 0;       /* how many slots filled so far */
static int   s_hist_index = 0;       /* next slot to overwrite (circular) */

/* Exponential moving average state */
static float s_smoothed_v = 0.0f;
static bool  s_smoothed_init = false;

/* "Last displayed" percentage — public return value from getter */
static uint8_t s_last_displayed_pct = 255;

/* Two-reading confirmation state for large jumps */
static uint8_t s_pending_jump_pct = 255;
static bool    s_pending_active = false;

/* Sampling cadence: every 2nd call (called every ~30s → samples every ~60s) */
static uint8_t s_cadence_counter = 0;

/* Spinlock — protects all above state */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* Threshold for "large jump" that requires two-reading confirmation */
#define JUMP_THRESHOLD   6   /* percentage points */

/**
 * @brief Comparator for qsort used in median calculation.
 */
static int cmp_float(const void *a, const void *b)
{
    float fa = *(const float *)a;
    float fb = *(const float *)b;
    if (fa < fb) return -1;
    if (fa > fb) return  1;
    return 0;
}

/**
 * @brief Compute the median of a float array of length n.
 *        Sorts in-place.
 */
static float median_filter(float *arr, int n)
{
    qsort(arr, n, sizeof(float), cmp_float);
    if (n % 2 == 1) {
        return arr[n / 2];
    }
    /* Even count: average of two middle values */
    return (arr[n / 2 - 1] + arr[n / 2]) * 0.5f;
}

esp_err_t battery_init(void)
{
    /* ADC1 oneshot init */
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&init_cfg, &adc1_handle), TAG, "adc1 init");

    /* GPIO4 config: ADC1_CHANNEL_3, 12-bit, DB_12 attenuation */
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_3, &chan_cfg),
                        TAG, "adc1 channel 3 config");

    /* Curve-fitting calibration */
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    esp_err_t e = adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali_handle);
    if (e == ESP_OK) {
        cali_enabled = true;
    } else {
        ESP_LOGW(TAG, "calibration failed, fallback to raw estimate");
    }
    return ESP_OK;
}

/**
 * @brief Perform a single ADC read and return the battery voltage in volts.
 *        Uses the same 16-sample average as before.
 */
static float battery_read_voltage(void)
{
    if (adc1_handle == NULL) return -1.0f;

    /* Read 16 samples, average to reduce noise */
    int raw = 0;
    for (int i = 0; i < 16; i++) {
        int v;
        if (adc_oneshot_read(adc1_handle, ADC_CHANNEL_3, &v) == ESP_OK) {
            raw += v;
        }
    }
    raw /= 16;

    /* Convert to mV */
    int mv = 0;
    if (cali_enabled) {
        adc_cali_raw_to_voltage(cali_handle, raw, &mv);
    } else {
        /* Fallback: 12-bit @ DB_12 -> ~0-2500 mV range */
        mv = raw * 2500 / 4095;
    }

    /* Battery voltage = ADC mV x 3 (on-board voltage divider) */
    return 0.001f * mv * 3.0f;
}

void battery_sample_if_due(void)
{
    /* ---- Cadence: only sample every 2nd call ---- */
    s_cadence_counter++;
    if (s_cadence_counter % 2 != 0) {
        return;  /* skip this call, no sampling */
    }

    /* Small settle delay after WIFI_PS_MIN_MODEM re-enable to let
     * the radio enter low-power state before sampling. */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* ---- 1. Raw ADC read ---- */
    float v = battery_read_voltage();
    if (v < 0.0f) {
        ESP_LOGW(TAG, "ADC read failed");
        return;
    }

    /* ---- 2. Push into rolling history buffer ---- */
    s_history[s_hist_index] = v;
    s_hist_index = (s_hist_index + 1) % HISTORY_SIZE;
    if (s_hist_count < HISTORY_SIZE) {
        s_hist_count++;
    }

    int hist_len = s_hist_count;

    /* ---- 3. Compute median of history ---- */
    float copy[HISTORY_SIZE];
    taskENTER_CRITICAL(&s_lock);
    memcpy(copy, s_history, sizeof(float) * hist_len);
    taskEXIT_CRITICAL(&s_lock);

    float median_v = median_filter(copy, hist_len);

    /* ---- 4. Exponential moving average ---- */
    if (!s_smoothed_init) {
        s_smoothed_v = median_v;
        s_smoothed_init = true;
    } else {
        /* α = 0.2 — light smoothing that preserves genuine trends */
        s_smoothed_v = 0.8f * s_smoothed_v + 0.2f * median_v;
    }

    /* ---- 5. Map to percentage via LUT ---- */
    uint8_t raw_pct = battery_voltage_to_percent(s_smoothed_v);

    /* ---- 6. Two-reading confirmation for large jumps ---- */
    uint8_t current_displayed;
    taskENTER_CRITICAL(&s_lock);
    current_displayed = s_last_displayed_pct;
    taskEXIT_CRITICAL(&s_lock);

    uint8_t new_pct;

    if (current_displayed == 255) {
        /* First valid reading — always accept */
        new_pct = raw_pct;
    } else {
        int delta = (int)raw_pct - (int)current_displayed;
        if (delta < 0) delta = -delta;

        if (delta > JUMP_THRESHOLD) {
            /* Large jump — require confirmation */
            if (s_pending_active) {
                /* Check if current reading agrees with the pending jump
                 * (within ±2 points, same direction). */
                int pending_delta = (int)raw_pct - (int)current_displayed;
                int pend_sign = (pending_delta >= 0) ? 1 : -1;
                int expected_delta = (int)s_pending_jump_pct - (int)current_displayed;
                int exp_sign = (expected_delta >= 0) ? 1 : -1;

                if (pend_sign == exp_sign) {
                    int diff = (int)raw_pct - (int)s_pending_jump_pct;
                    if (diff < 0) diff = -diff;
                    if (diff <= 2) {
                        /* Confirmed — accept */
                        new_pct = raw_pct;
                        s_pending_active = false;
                        s_pending_jump_pct = 255;
                    } else {
                        /* Disagreement — clear pending, keep old value */
                        s_pending_active = false;
                        s_pending_jump_pct = 255;
                        new_pct = current_displayed;
                    }
                } else {
                    /* Direction reversed — clear pending, keep old */
                    s_pending_active = false;
                    s_pending_jump_pct = 255;
                    new_pct = current_displayed;
                }
            } else {
                /* First large jump — store as pending, keep old */
                s_pending_jump_pct = raw_pct;
                s_pending_active = true;
                new_pct = current_displayed;
            }
        } else {
            /* Small/normal change — accept immediately */
            new_pct = raw_pct;
            /* Clear any pending state */
            s_pending_active = false;
            s_pending_jump_pct = 255;
        }
    }

    /* ---- 7. Store accepted value ---- */
    taskENTER_CRITICAL(&s_lock);
    s_last_displayed_pct = new_pct;
    taskEXIT_CRITICAL(&s_lock);
}

uint8_t battery_get_percentage(void)
{
    uint8_t pct;
    taskENTER_CRITICAL(&s_lock);
    pct = s_last_displayed_pct;
    taskEXIT_CRITICAL(&s_lock);
    return pct;
}