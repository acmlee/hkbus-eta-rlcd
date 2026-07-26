#pragma once

#include <stdint.h>

#define BUTTON_GPIO 18   /** KEY button on Waveshare ESP32-S3-RLCD-4.2 */

/**
 * @brief Configure GPIO18 (KEY button) as input with internal pull-up
 *        and falling-edge interrupt. Call once at boot.
 */
void button_init(void);

/**
 * @brief Atomically return the number of presses since the last call,
 *        then reset the counter to 0. Returns 0 if no presses.
 *
 * Multiple bounces within one poll interval collapse into a single
 * non-zero return at the consumer side.
 */
uint32_t button_consume_presses(void);

/**
 * @brief Non-destructive peek of current press count (for debug).
 */
uint32_t button_get_press_count(void);