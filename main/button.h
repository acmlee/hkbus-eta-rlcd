#pragma once

#include <stdint.h>

#define BUTTON_GPIO 18   /** KEY button on Waveshare ESP32-S3-RLCD-4.2 */

/**
 * @brief Configure GPIO18 (KEY button) as input with internal pull-up
 *        and low-level interrupt + light-sleep wakeup. Call once at boot.
 */
void button_init(void);

/**
 * @brief Return the number of complete press-and-release cycles since the
 *        last call (0 or 1), deduping the level-triggered ISR that fires
 *        continuously while the button is held.
 *
 * The GPIO is a light-sleep wakeup source, so presses are never missed even
 * when the SoC is asleep; the counter is latched by the ISR and consumed
 * here (any non-zero return means "at least one press since last poll").
 */
uint32_t button_consume_presses(void);

/**
 * @brief Non-destructive peek of whether a press is pending (0 or 1, debug).
 */
uint32_t button_get_press_count(void);