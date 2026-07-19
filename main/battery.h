#pragma once
#include <stdint.h>
#include "esp_err.h"

/**
 * @brief Initialise ADC1 on GPIO4 for battery voltage reading.
 *        Uses the same config as the Waveshare FactoryProgram (adc_bsp.cpp).
 *        ADC1 is safe to use while Wi-Fi is active (unlike ADC2).
 */
esp_err_t battery_init(void);

/**
 * @brief Sample the battery ADC if the sampling cadence is due.
 *
 * Must be called from eta_fetch_task during a confirmed Wi-Fi-idle
 * window (after WIFI_PS_MIN_MODEM is re-enabled, before vTaskDelay).
 * Maintains internal rolling history + median filter + EMA + outlier
 * rejection.  Safe to call from any task — uses a spinlock internally.
 *
 * Cadence: every ~60 s (every 2nd call if called every 30 s).
 */
void battery_sample_if_due(void);

/**
 * @brief Return the last accepted (filtered + confirmed) battery
 *        percentage for display.
 *
 * Returns 0-100 on success, or 255 on error (no valid reading yet).
 * Does NOT trigger an ADC read — safe to call from display_task.
 */
uint8_t battery_get_percentage(void);