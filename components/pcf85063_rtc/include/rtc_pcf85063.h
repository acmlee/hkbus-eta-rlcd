#ifndef RTC_PCF85063_H
#define RTC_PCF85063_H

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PCF85063 RTC bridge — C API into the vendored Waveshare PCF85063 driver.
 *
 * The RTC keeps wall-clock time across power cycles via the board's RTC
 * backup battery. The system clock is restored from it at boot and the
 * RTC is (re)written on every successful SNTP sync, including the daily
 * 06:00 resync from stdtime.gov.hk. All times are Asia/Hong_Kong (HKT)
 * wall time; no DST (HK has none).
 *
 * Any failure keeps the firmware in today's SNTP-only mode: every
 * function returns false / no-ops and only logs a warning.
 */

/* All functions are prefixed rtc_pcf85063_ to avoid colliding with
 * ESP-IDF's own rtc_* symbols in esp_hw_support (e.g. rtc_init()). */

/* Initialise the PCF85063 on I2C0 (SDA=GPIO13, SCL=GPIO14), 300 kHz.
 * Returns true when the chip is present and running. Idempotent. */
bool rtc_pcf85063_init(void);

/* Read the RTC into a broken-down HKT time. Returns false when the RTC
 * is not ready or the stored date fails a plausibility check (first
 * boot, dead backup battery, or failed read). */
bool rtc_pcf85063_get_time(struct tm *out);

/* Write a broken-down HKT time to the RTC. */
bool rtc_pcf85063_set_time(const struct tm *in);

/* Push the current system clock (HKT wall time) into the RTC. No-op
 * unless the RTC is ready and the system clock is valid (post-SNTP).
 * Intended to be called from the SNTP sync callback, so the RTC is
 * updated every successful sync (boot and the daily 06:00 resync). */
void rtc_pcf85063_store_system_time(void);

/* Pull the RTC value into the system clock via settimeofday(). Returns
 * false when the RTC is not ready or its value is implausible. */
bool rtc_pcf85063_restore_system_time(void);

#ifdef __cplusplus
}
#endif

#endif /* RTC_PCF85063_H */
